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
package io.netty.channel.epoll;

import java.net.InetSocketAddress;

/**
 * Act as special {@link InetSocketAddress} to be able to easily pass all needed data from JNI without the need
 * to create more objects then needed.
 */
final class DatagramSocketAddress extends InetSocketAddress {

    private static final long serialVersionUID = 1348596211215015739L;

    // holds the amount of received bytes
    final int receivedAmount;

    DatagramSocketAddress(String addr, int port, int receivedAmount) {
        super(addr, port);
        this.receivedAmount = receivedAmount;
    }
}
