#ifndef MY_FACE_DOOR_CONTROL_CONFIG_H
#define MY_FACE_DOOR_CONTROL_CONFIG_H

/*
 * 编译期门锁执行机构配置。
 * 当前默认值映射到板载 LED1，用于在无继电器/蜂鸣器时显示“门打开”状态。
 * LED1: BANK0_GPIO6，低电平点亮。
 */

#ifndef FACE_DOOR_ENABLE
#define FACE_DOOR_ENABLE 1
#endif

#ifndef FACE_DOOR_RELAY_PIN
#define FACE_DOOR_RELAY_PIN 6
#endif

#ifndef FACE_DOOR_RELAY_ACTIVE_HIGH
#define FACE_DOOR_RELAY_ACTIVE_HIGH 0
#endif

#ifndef FACE_DOOR_BUZZER_PIN
#define FACE_DOOR_BUZZER_PIN -1
#endif

#ifndef FACE_DOOR_BUZZER_ACTIVE_HIGH
#define FACE_DOOR_BUZZER_ACTIVE_HIGH 1
#endif

#ifndef FACE_DOOR_HOLD_MS
#define FACE_DOOR_HOLD_MS 3000
#endif

#ifndef FACE_DOOR_VERIFY_READBACK
#define FACE_DOOR_VERIFY_READBACK 0
#endif

#endif
