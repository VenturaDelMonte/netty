/*
 * Copyright 2015 The Netty Project
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
#include <jni.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "io_netty_channel_unix_FileDescriptor.h"


static jint netty_unix_filedescriptor_close(JNIEnv* env, jclass clazz, jint fd) {
   if (close(fd) < 0) {
       return -errno;
   }
   return 0;
}

static char* netty_unix_util2_prepend(const char* prefix, const char* str) {
    if (prefix == NULL) {
        char* result = (char*) malloc(sizeof(char) * (strlen(str) + 1));
        strcpy(result, str);
        return result;
    }
    char* result = (char*) malloc(sizeof(char) * (strlen(prefix) + strlen(str) + 1));
    strcpy(result, prefix);
    strcat(result, str);
    return result;
}

jint netty_unix_util2_register_natives(JNIEnv* env, const char* packagePrefix, const char* className, const JNINativeMethod* methods, jint numMethods) {
    char* nettyClassName = netty_unix_util2_prepend(packagePrefix, className);
    jclass nativeCls = (*env)->FindClass(env, nettyClassName);
    free(nettyClassName);
    nettyClassName = NULL;
    if (nativeCls == NULL) {
        return JNI_ERR;
    }
    return (*env)->RegisterNatives(env, nativeCls, methods, numMethods);
}

static const JNINativeMethod method_table[] = {
    { "close", "(I)I", (void *) netty_unix_filedescriptor_close },
};
static const jint method_table_size = sizeof(method_table) / sizeof(method_table[0]);

jint netty_unix_filedescriptor_JNI_OnLoad(JNIEnv* env, const char* packagePrefix) {
    if (netty_unix_util2_register_natives(env, packagePrefix, "io/netty/channel/unix/FileDescriptor", method_table, method_table_size) != 0) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}


void netty_unix_filedescriptor_JNI_OnUnLoad(JNIEnv* env) { }

