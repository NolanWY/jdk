#include "jni.h"
#include "jvm.h"

#include "java_util_JitValidator.h"

static JNINativeMethod methods[] = {
        {"run", "()V", (void *) &JVM_ValidateJitCode},
};

JNIEXPORT void JNICALL
Java_java_util_JitValidator_registerNatives(JNIEnv *env, jclass cls)
{
  (*env)->RegisterNatives(env, cls,
                          methods, sizeof(methods)/sizeof(methods[0]));
}
