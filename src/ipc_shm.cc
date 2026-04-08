#include "ipc_shm.h"
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

static uint32_t ipc_seq;

int ipc_shm_alloc(size_t size, void **out_vaddr)
{
    size_t key = (size_t)getpid() ^ ((size_t)++ipc_seq << 20);
    int shmid = lwp_shmget(key, size, 1);
    if (shmid < 0)
        return -1;
    void *p = lwp_shmat(shmid, NULL);
    if (p == RT_NULL)
    {
        lwp_shmrm(shmid);
        return -1;
    }
    *out_vaddr = p;
    return shmid;
}

void ipc_shm_free(int shmid)
{
    if (shmid >= 0)
        lwp_shmrm(shmid);
}

int ipc_request_encode_buffer(void *dst, size_t dst_capacity, ipc_cmd_t cmd, const uint8_t *frame_rgb,
                              size_t frame_len, uint32_t tensor_c, uint32_t tensor_h, uint32_t tensor_w,
                              const char *register_name, uint32_t seq)
{
    size_t need = sizeof(ipc_req_hdr_t) + frame_len;
    if (dst_capacity < need)
        return -1;

    ipc_req_hdr_t *hdr = (ipc_req_hdr_t *)dst;
    hdr->magic = IPC_MAGIC;
    hdr->cmd = cmd;
    hdr->seq = seq;
    hdr->frame_bytes = (uint32_t)frame_len;
    hdr->tensor_c = tensor_c;
    hdr->tensor_h = tensor_h;
    hdr->tensor_w = tensor_w;
    memset(hdr->register_name, 0, sizeof(hdr->register_name));
    if (register_name)
        strncpy(hdr->register_name, register_name, IPC_NAME_MAX - 1);

    if (frame_len && frame_rgb)
        memcpy((uint8_t *)dst + sizeof(ipc_req_hdr_t), frame_rgb, frame_len);

    return 0;
}

int ipc_pack_request(ipc_cmd_t cmd, const uint8_t *frame_rgb, size_t frame_len, uint32_t tensor_c,
                     uint32_t tensor_h, uint32_t tensor_w, const char *register_name, uint32_t seq,
                     int *out_shmid)
{
    size_t total = sizeof(ipc_req_hdr_t) + frame_len;
    void *p = nullptr;
    int shmid = ipc_shm_alloc(total, &p);
    if (shmid < 0)
        return -1;

    if (ipc_request_encode_buffer(p, total, cmd, frame_rgb, frame_len, tensor_c, tensor_h, tensor_w, register_name,
                                  seq) != 0)
    {
        lwp_shmdt(p);
        ipc_shm_free(shmid);
        return -1;
    }

    lwp_shmdt(p);
    *out_shmid = shmid;
    return 0;
}
