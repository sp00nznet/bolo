#ifndef EGA_H
#define EGA_H
#include <stdint.h>
void    ega_port_write(uint16_t port, uint8_t value);
uint8_t ega_port_read(uint16_t port);
void    ega_write8(uint32_t off, uint8_t value);
uint8_t ega_read8(uint32_t off);
void    ega_set_mode(void);
void    ega_dump(const char *path);
void    ega_render(uint32_t *out);          /* 640x350 RGBA of the shown page */
void    ega_attr_reset(void);
void    ega_set_palette(int reg, uint8_t v);
#endif
