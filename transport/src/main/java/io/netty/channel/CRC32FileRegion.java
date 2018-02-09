/*
 * Copyright 2014 The Netty Project
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
package io.netty.channel;

import io.netty.util.AbstractReferenceCounted;
import io.netty.util.ReferenceCountedFileChannel;
import io.netty.util.internal.logging.InternalLogger;
import io.netty.util.internal.logging.InternalLoggerFactory;

import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.channels.WritableByteChannel;

public abstract class CRC32FileRegion extends AbstractReferenceCounted implements FileRegion {

    protected static final InternalLogger logger = InternalLoggerFactory.getInstance(CRC32FileRegion.class);

    protected final long position;
    protected final long count;
    protected long transfered;
    protected final FileChannel file;
    protected final ReferenceCountedFileChannel refChannel;


    /**
     * Create a new instance
     *
     * @param refChannel the {@link ReferenceCountedFileChannel} which should be transfered
     * @param position  the position from which the transfer should start
     * @param count     the number of bytes to transfer
     */
    public CRC32FileRegion(ReferenceCountedFileChannel refChannel, long position, long count) {
        if (refChannel == null) {
            throw new NullPointerException("refChannel");
        }
        if (position < 0) {
            throw new IllegalArgumentException("position must be >= 0 but was " + position);
        }
        if (count < 0) {
            throw new IllegalArgumentException("count must be >= 0 but was " + count);
        }
        this.file = refChannel.channel();
        this.refChannel = refChannel;
        this.position = position;
        this.count = count;
    }

    @Override
    public long position() {
        return position;
    }

    @Override
    public long transfered() {
        return transfered;
    }

    @Override
    public long count() {
        return count;
    }

    @Override
    public abstract long transferTo(WritableByteChannel target, long position) throws IOException;

    @Override
    protected void deallocate() {
        refChannel.release();
    }
}
