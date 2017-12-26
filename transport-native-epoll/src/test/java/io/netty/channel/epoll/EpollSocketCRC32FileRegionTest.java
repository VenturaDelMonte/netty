/*
 * Copyright 2012 The Netty Project
 *
 * The Netty Project licenses this file to you under the Apache License,
 * version 2.0 (the "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at:
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
package io.netty.channel.epoll;

import io.netty.bootstrap.Bootstrap;
import io.netty.bootstrap.ServerBootstrap;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.channel.CRC32FileRegion;
import io.netty.channel.Channel;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandler;
import io.netty.channel.ChannelOption;
import io.netty.channel.FileRegion;
import io.netty.channel.SimpleChannelInboundHandler;
import io.netty.testsuite.transport.TestsuitePermutation;
import io.netty.testsuite.transport.socket.AbstractSocketTest;
import io.netty.util.ReferenceCountedFileChannel;
import io.netty.util.internal.ThreadLocalRandom;
import org.junit.Ignore;
import org.junit.Test;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.channels.WritableByteChannel;
import java.util.List;
import java.util.Random;
import java.util.concurrent.atomic.AtomicReference;

import static org.hamcrest.CoreMatchers.is;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertThat;

public class EpollSocketCRC32FileRegionTest extends AbstractSocketTest {

    static final byte[] data = new byte[32768];

    @Override
    protected List<TestsuitePermutation.BootstrapComboFactory<ServerBootstrap, Bootstrap>> newFactories() {
        return EpollSocketTestPermutation.INSTANCE.socket().subList(0, 1);
    }

    @Test
    @Ignore
    public void testCRC32FileRegion() throws Throwable {
        run();
    }

    public void testCRC32FileRegion(ServerBootstrap sb, Bootstrap cb) throws Throwable {
        testCRC32FileRegion0(sb, cb, false, true);
    }

    private void testCRC32FileRegion0(
            ServerBootstrap sb,
            Bootstrap cb,
            boolean voidPromise,
            final boolean autoRead) throws Throwable {

        final int bufferSize = 1024;
        final File file = File.createTempFile("netty-", ".tmp");
        file.deleteOnExit();

        final FileOutputStream out = new FileOutputStream(file);
        final Random random = ThreadLocalRandom.current();

        // Prepend random data which will not be transferred, so that we can test non-zero start offset
//        final int startOffset = random.nextInt(8192);
//        for (int i = 0; i < startOffset; i ++) {
//            out.write(random.nextInt());
//        }

        // .. and here comes the real data to transfer.
        out.write(data, 0, data.length);

        // .. and then some extra data which is not supposed to be transferred.
//        for (int i = random.nextInt(8192); i > 0; i --) {
//            out.write(random.nextInt());
//        }

        out.close();

        ChannelInboundHandler ch = new SimpleChannelInboundHandler<Object>() {
            @Override
            public void channelReadComplete(ChannelHandlerContext ctx) throws Exception {
                if (!autoRead) {
                    ctx.read();
                }
            }

            @Override
            public void channelRead0(ChannelHandlerContext ctx, Object msg) throws Exception {
            }

            @Override
            public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) throws Exception {
                ctx.close();
            }
        };
        TestHandler sh = new TestHandler(autoRead);

        sb.childOption(ChannelOption.WRITE_BUFFER_HIGH_WATER_MARK, 32768);
        sb.childOption(ChannelOption.WRITE_BUFFER_LOW_WATER_MARK, 32768);
        sb.option(ChannelOption.SO_SNDBUF, 32768);
        sb.option(ChannelOption.SO_RCVBUF, 32768);
        cb.option(ChannelOption.SO_SNDBUF, 32768);
        cb.option(ChannelOption.SO_RCVBUF, 32768);
        sb.childOption(EpollChannelOption.CREATE_CRC32_SERVER, true);
        cb.option(EpollChannelOption.CREATE_CRC32_SERVER, true);

        sb.childHandler(sh);
        cb.handler(ch);

        Channel sc = sb.bind().sync().channel();

        Channel cc = cb.connect().sync().channel();
        FileRegion region = new CRC32FileRegion(
                new ReferenceCountedFileChannel(new FileInputStream(file).getChannel()),
                0, data.length) {
            @Override
            public long transferTo(WritableByteChannel target, long position) throws IOException {
                return 0;
            }

            @Override
            public long checksumLength() {
                return 4;
            }

            @Override
            public long count() {
                return data.length + 4;
            }
        };
        FileRegion emptyRegion = new CRC32FileRegion(new ReferenceCountedFileChannel(
                new FileInputStream(file).getChannel()), 0, 0) {
            @Override
            public long transferTo(WritableByteChannel target, long position) throws IOException {
                return 0;
            }

            @Override
            public long checksumLength() {
                return 4;
            }
        };

        // Do write ByteBuf and then FileRegion to ensure that mixed writes work
        // Also, write an empty FileRegion to test if writing an empty FileRegion does not cause any issues.
        //
        // See https://github.com/netty/netty/issues/2769
        //     https://github.com/netty/netty/issues/2964
        if (voidPromise) {
            assertEquals(cc.voidPromise(), cc.write(Unpooled.wrappedBuffer(data, 0, bufferSize), cc.voidPromise()));
            assertEquals(cc.voidPromise(), cc.write(emptyRegion, cc.voidPromise()));
            assertEquals(cc.voidPromise(), cc.writeAndFlush(region, cc.voidPromise()));
        } else {
            //assertNotEquals(cc.voidPromise(), cc.write(Unpooled.wrappedBuffer(data, 0, bufferSize)));
//            assertNotEquals(cc.voidPromise(), cc.writeAndFlush(emptyRegion));
            assertNotEquals(cc.voidPromise(), cc.writeAndFlush(region));
        }

        while (sh.counter < data.length) {
            if (sh.exception.get() != null) {
                break;
            }

            try {
                Thread.sleep(50);
            } catch (InterruptedException e) {
                // Ignore.
            }
        }

        sh.channel.close().sync();
        cc.close().sync();
        sc.close().sync();

        if (sh.exception.get() != null && !(sh.exception.get() instanceof IOException)) {
            throw sh.exception.get();
        }

        if (sh.exception.get() != null) {
            throw sh.exception.get();
        }

        // Make sure we did not receive more than we expected.
        assertThat(sh.counter, is(data.length));
    }

    private static class TestHandler extends SimpleChannelInboundHandler<ByteBuf> {
        private final boolean autoRead;
        volatile Channel channel;
        final AtomicReference<Throwable> exception = new AtomicReference<Throwable>();
        volatile int counter;

        TestHandler(boolean autoRead) {
            this.autoRead = autoRead;
        }

        @Override
        public void channelActive(ChannelHandlerContext ctx)
                throws Exception {
            channel = ctx.channel();
        }

        @Override
        public void channelRead0(ChannelHandlerContext ctx, ByteBuf in) throws Exception {
            byte[] actual = new byte[in.readableBytes()];
            in.readBytes(actual);

            int lastIdx = counter;
   //         for (int i = 0; i < actual.length; i ++) {
 //               assertEquals(data[i + lastIdx], actual[i]);
     //       }
            counter += actual.length;
        }

        @Override
        public void channelReadComplete(ChannelHandlerContext ctx) throws Exception {
            if (!autoRead) {
                ctx.read();
            }
        }

        @Override
        public void exceptionCaught(ChannelHandlerContext ctx,
                                    Throwable cause) throws Exception {
            if (exception.compareAndSet(null, cause)) {
                ctx.close();
            }
        }
    }
}
