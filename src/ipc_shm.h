#ifndef MY_FACE_IPC_SHM_H
#define MY_FACE_IPC_SHM_H

#include "ipc_proto.h"
#include <stddef.h>
#include <stdint.h>

#include <fcntl.h>
#include "ipc_lwp_user.h"

int ipc_shm_alloc(size_t size, void **out_vaddr);
void ipc_shm_free(int shmid);

int ipc_pack_request(ipc_cmd_t cmd, const uint8_t *frame_rgb, size_t frame_len, uint32_t tensor_c,
                     uint32_t tensor_h, uint32_t tensor_w, const char *register_name, uint32_t seq,
                     int *out_shmid);

#endif
