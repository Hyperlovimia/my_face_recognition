/*
 * 用户态 IPC 声明（与 rtsmart/kernel/.../lwp_ipc.h、userapps/sdk/.../lwp_shm.h 二进制兼容）。
 * 不 #include <rtthread.h>，避免拉入 rtdevice.h 等，便于在仅配置 MPP/OpenCV 的交叉编译环境中通过编译。
 */
#ifndef MY_FACE_IPC_LWP_USER_H
#define MY_FACE_IPC_LWP_USER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RT_NULL
#define RT_NULL ((void *)0)
#endif

typedef long rt_err_t;

enum
{
    RT_CHANNEL_RAW,
    RT_CHANNEL_BUFFER
};

struct rt_channel_msg
{
    void *sender;
    int type;
    union
    {
        struct chbuf
        {
            void *buf;
            size_t length;
        } b;
        void *d;
    } u;
};
typedef struct rt_channel_msg *rt_channel_msg_t;

/* 实现在 ipc_lwp_syscall.c（syscall 封装，供静态链接） */
int rt_channel_open(const char *name, int flags);
rt_err_t rt_channel_close(int fd);
rt_err_t rt_channel_send(int fd, rt_channel_msg_t data);
rt_err_t rt_channel_send_recv(int fd, rt_channel_msg_t data, rt_channel_msg_t data_ret);
rt_err_t rt_channel_reply(int fd, rt_channel_msg_t data);
rt_err_t rt_channel_recv(int fd, rt_channel_msg_t data);

int lwp_shmget(size_t key, size_t size, int create);
int lwp_shmrm(int id);
void *lwp_shmat(int id, void *shm_vaddr);
int lwp_shmdt(void *shm_vaddr);

#ifdef __cplusplus
}
#endif

#endif
