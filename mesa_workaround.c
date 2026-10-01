/*
 * 规避 QQ 自带 zstd 与 Mesa 着色器缓存的符号冲突。
 *
 * QQ 的 libAVSDKPlugin.so 静态打包了一份 zstd 并导出全部 ZSTD_* 符号。
 * 共享屏幕时 broadcast-core.so 初始化 OpenGL（glXChooseVisual），
 * Mesa 读取磁盘着色器缓存会调用系统 libzstd 的 ZSTD_decompress，
 * 而 libzstd 内部对 ZSTD_decompressDCtx 等的调用被 QQ 那份同名符号截走，
 * 两个版本结构体不兼容，ppapi 进程随即以 SIGTRAP 崩溃，表现为点「确定」后没有任何反应。
 *
 * 关闭 Mesa 的磁盘着色器缓存即可避开解压调用。这里在库加载时设置环境变量，
 * QQ 之后创建的所有子进程都会继承；用户已显式设置时不覆盖。
 */
#include <stdlib.h>

__attribute__((constructor))
static void qwlss_disable_mesa_shader_cache(void)
{
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 0);
}
