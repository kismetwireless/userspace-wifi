/* Emulated kernel header; the upstream wireless headers are from v7.3 */
#ifndef __KC_LINUX_VERSION_H
#define __KC_LINUX_VERSION_H
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#define LINUX_VERSION_CODE KERNEL_VERSION(7, 3, 0)
#endif
