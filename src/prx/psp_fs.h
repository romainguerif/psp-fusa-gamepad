/* PSP Bridge — Memory Stick access (sceIo) for the file channel */
#ifndef PSP_FS_H
#define PSP_FS_H

#include "../common/bridge_proto.h"

const BridgeFs *psp_fs(void);
/* Closes the file kept open between reads (host gone, stop) */
void psp_fs_close(void);

#endif
