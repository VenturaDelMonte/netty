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
package io.netty.buffer;

import org.junit.Test;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

import static org.junit.Assert.*;

/**
 * Tests big-endian direct channel buffers
 */
public class PooledBigEndianDirectByteBufTest extends AbstractByteBufTest {

    private ByteBuf buffer;

    @Override
    protected ByteBuf newBuffer(int length) {
        buffer = PooledByteBufAllocator.DEFAULT.directBuffer(length);
        assertSame(ByteOrder.BIG_ENDIAN, buffer.order());
        assertEquals(0, buffer.writerIndex());
        return buffer;
    }

    @Override
    protected ByteBuf[] components() {
        return new ByteBuf[] { buffer };
    }

    @Test
    public void testWriteBytesWithByteBuffer() {
        int nioBuffSize = 4 * 256;
        int nettyBuffSize = nioBuffSize + 8;
        ByteBuffer nioBuffer = ByteBuffer.allocateDirect(nioBuffSize);
        for (int i = 0; i < 256; i++) {
            nioBuffer.putInt(i);
        }
        ByteBuf nettyBuff = null;
        try {
            nettyBuff = newBuffer(nettyBuffSize);
            nettyBuff.writeLong(23);
            nettyBuff.writeBytes(nioBuffer, 0, nioBuffSize);
            nettyBuff.readerIndex(12);
            int val = nettyBuff.readInt();
            assertEquals(1, val);
            nettyBuff.readerIndex(nettyBuffSize - 4);
            val = nettyBuff.readInt();
            assertEquals(255, val);
        } finally {
            if (nettyBuff != null) {
                nettyBuff.release();
            }
        }
    }
}
