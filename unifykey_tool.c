#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define UNIFYKEYS_DIR "/sys/class/unifykeys"
#define BUF_SIZE      4096

/* 向 sysfs 节点写入数据（自动追加换行符，模拟 echo） */
static int sysfs_write(const char *node, const char *data) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", UNIFYKEYS_DIR, node);
    
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) {
        fprintf(stderr, "  [!] 打开 %s 失败: %s\n", path, strerror(errno));
        return -1;
    }
    
    char buf[512];
    size_t len = snprintf(buf, sizeof(buf), "%s\n", data);
    if (write(fd, buf, len) != (ssize_t)len) {
        fprintf(stderr, "  [!] 写入 %s 失败: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* 从 sysfs 节点读取数据 */
static int sysfs_read(const char *node, char *buf, size_t len) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", UNIFYKEYS_DIR, node);
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "  [!] 打开 %s 失败: %s\n", path, strerror(errno));
        return -1;
    }
    
    ssize_t r = read(fd, buf, len - 1);
    close(fd);
    if (r < 0) {
        fprintf(stderr, "  [!] 读取 %s 失败: %s\n", path, strerror(errno));
        return -1;
    }
    buf[r] = '\0';

    // 清除末尾换行符，仅移除最后一行尾部的换行符，避免截断多行输出
    if (r > 0 && buf[r - 1] == '\n') {
        buf[r - 1] = '\0';
    }
    return 0;
}

static void delay_ms(int ms) {
    usleep(ms * 1000);
}

/* 初始化驱动上下文 */
static int unify_init() {
    return sysfs_write("attach", "1");
}

/* 解锁 sysfs 操作权限 */
static int unify_unlock() {
    return sysfs_write("lock", "0");
}

/* 锁定 sysfs 操作权限 */
static int unify_lock() {
    return sysfs_write("lock", "1");
}

int main(int argc, char *argv[]) {
    if (getuid() != 0) {
        fprintf(stderr, "[!] 错误: 需要 root 权限。请执行: adb root && adb shell su\n");
        return EXIT_FAILURE;
    }

    struct stat st;
    if (stat(UNIFYKEYS_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "[!] 错误: 未找到 %s (请确认 BSP 路径是否为 /sys/class/unifykey)\n", UNIFYKEYS_DIR);
        return EXIT_FAILURE;
    }

    printf("========================================\n");
    printf(" Amlogic Unifykeys Tool (Android 9 32bit)\n");
    printf("========================================\n");

    /* 统一初始化 */
    if (unify_init() != 0) {
        fprintf(stderr, "[!] attach 失败，请确认 unifykeys 驱动已加载\n");
        return EXIT_FAILURE;
    }
    delay_ms(200);
    
    if (unify_unlock() != 0) {
        fprintf(stderr, "[!] unlock 失败\n");
        return EXIT_FAILURE;
    }
    delay_ms(200);

    /* 命令解析 */
    const char *cmd = (argc > 1) ? argv[1] : "list";
    int ret = 0;

    if (strcmp(cmd, "list") == 0) {
        printf("\n[模式] 列出所有密钥\n");
        char buf[BUF_SIZE];
        if (sysfs_read("list", buf, sizeof(buf)) == 0) {
            printf(">> 可用密钥列表:\n%s\n", buf);
        } else {
            ret = -1;
        }

    } else if (strcmp(cmd, "read") == 0) {
        if (argc < 3) {
            fprintf(stderr, "[!] 用法: %s read <key_name>\n", argv[0]);
            ret = -1;
        } else {
            const char *key_name = argv[2];
            printf("\n[模式] 读取密钥: %s\n", key_name);
            
            if (sysfs_write("name", key_name) != 0) {
                fprintf(stderr, "  [!] 设置 name 失败\n");
                ret = -1;
            } else {
                delay_ms(150); // 等待内核加载密钥上下文
                char buf[BUF_SIZE];
                if (sysfs_read("read", buf, sizeof(buf)) == 0) {
                    printf(">> 读取结果: %s\n", buf);
                } else {
                    fprintf(stderr, "  [!] 读取失败 (可能 key 不存在或硬件保护)\n");
                    ret = -1;
                }
            }
        }

    } else if (strcmp(cmd, "write") == 0) {
        if (argc < 4 || (argc - 2) % 2 != 0) {
            fprintf(stderr, "[!] 用法: %s write <name> <value> [name2 val2 ...]\n", argv[0]);
            ret = -1;
        } else {
            printf("\n[模式] 批量烧录密钥\n");
            for (int i = 2; i < argc - 1; i += 2) {
                const char *name = argv[i];
                const char *val  = argv[i+1];
                printf(">> 写入: %s = %s\n", name, val);

                if (sysfs_write("name", name) != 0) continue;
                delay_ms(100);
                
                if (sysfs_write("write", val) != 0) {
                    fprintf(stderr, "  [!] write 指令失败，跳过\n");
                    continue;
                }
                delay_ms(500); // OTP/eFuse 编程需要固化时间

                char rbuf[BUF_SIZE], ebuf[32];
                if (sysfs_read("read", rbuf, sizeof(rbuf)) == 0)
                    printf("  >> cat read : %s\n", rbuf);
                if (sysfs_read("exist", ebuf, sizeof(ebuf)) == 0)
                    printf("  >> cat exist: %s\n", ebuf);
                delay_ms(100);
            }
        }
    } else {
        fprintf(stderr, "[!] 未知命令: %s (支持: list, read, write)\n", cmd);
        ret = -1;
    }

    /* 统一锁定并退出 */
    printf("\n[收尾] 锁定密钥空间...\n");
    unify_lock();
    delay_ms(100);

    return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}