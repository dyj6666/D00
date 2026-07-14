#ifndef VAR_MANAGER_H
#define VAR_MANAGER_H
#include <stdint.h>

typedef enum {
    VAR_TYPE_UINT8,
    VAR_TYPE_INT16,
    VAR_TYPE_INT32,
    VAR_TYPE_FLOAT
} VarType;

typedef struct {
    uint16_t    id;         /* 唯一 ID，上位机识别 */
    const char  *name;      /* 变量名 */
    VarType     type;       /* 数据类型 */
    uint8_t     permission; /* 0:只读 1:可读写 */
    void        *ptr;       /* 指向实际变量的指针 */
} VarEntry;

void VAR_Init(void);
int  VAR_Register(uint16_t id, const char *name, VarType type, uint8_t perm, void *ptr);
int  VAR_Read(uint16_t id, void *buf, uint16_t *len);
int  VAR_Write(uint16_t id, const void *buf, uint16_t len);
void VAR_GetSubscribedList(uint16_t *ids, uint8_t *count);
void VAR_Subscribe(uint16_t id);
void VAR_ClearSubscriptions(void);
#endif