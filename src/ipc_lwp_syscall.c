/*
 * RT-Smart 用户态 IPC：通过 syscall 进入内核（与 sys/syscall_no.h 中 NRSYS 顺序一致）。
 * 静态链接时工具链 libc 未必提供 rt_channel_* / lwp_* 弱符号，故在此显式封装。
 */
#include <stddef.h>
#include <unistd.h>

#include "ipc_lwp_user.h"

/* 与 rtsmart/userapps/sdk/rt-thread/include/sys/syscall_no.h 中 NRSYS 枚举顺序一致（从 NRSYS(exit)=1 起算） */
#define SYS_CHANNEL_OPEN 44
#define SYS_CHANNEL_CLOSE 45
#define SYS_CHANNEL_SEND 46
#define SYS_CHANNEL_SEND_RECV_TIMEOUT 47
#define SYS_CHANNEL_REPLY 48
#define SYS_CHANNEL_RECV_TIMEOUT 49

#define SYS_SHMGET 55
#define SYS_SHMRM 56
#define SYS_SHMAT 57
#define SYS_SHMDT 58

#ifndef RT_WAITING_FOREVER
#define RT_WAITING_FOREVER (-1)
#endif

int rt_channel_open(const char *name, int flags)
{
    return (int)syscall(SYS_CHANNEL_OPEN, name, flags);
}

rt_err_t rt_channel_close(int fd)
{
    return (rt_err_t)syscall(SYS_CHANNEL_CLOSE, fd);
}

rt_err_t rt_channel_send(int fd, rt_channel_msg_t data)
{
    return (rt_err_t)syscall(SYS_CHANNEL_SEND, fd, data);
}

rt_err_t rt_channel_send_recv(int fd, rt_channel_msg_t data, rt_channel_msg_t data_ret)
{
    return (rt_err_t)syscall(SYS_CHANNEL_SEND_RECV_TIMEOUT, fd, data, data_ret, (long)RT_WAITING_FOREVER);
}

rt_err_t rt_channel_reply(int fd, rt_channel_msg_t data)
{
    return (rt_err_t)syscall(SYS_CHANNEL_REPLY, fd, data);
}

rt_err_t rt_channel_recv(int fd, rt_channel_msg_t data)
{
    return (rt_err_t)syscall(SYS_CHANNEL_RECV_TIMEOUT, fd, data, (long)RT_WAITING_FOREVER);
}

int lwp_shmget(size_t key, size_t size, int create)
{
    return (int)syscall(SYS_SHMGET, key, size, create);
}

int lwp_shmrm(int id)
{
    return (int)syscall(SYS_SHMRM, id);
}

void *lwp_shmat(int id, void *shm_vaddr)
{
    return (void *)syscall(SYS_SHMAT, id, shm_vaddr);
}

int lwp_shmdt(void *shm_vaddr)
{
    return (int)syscall(SYS_SHMDT, shm_vaddr);
}
