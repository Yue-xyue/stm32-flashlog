/*
 * flash.c — W25Q / XT25F 系列 SPI NOR flash driver
 */
#include "flash.h"

/* ---- 內部：命令碼 ---- */
#define CMD_READ_STATUS   0x05
#define CMD_WRITE_ENABLE  0x06
#define CMD_READ_DATA     0x03
#define CMD_PAGE_PROGRAM  0x02
#define CMD_SECTOR_ERASE  0x20
#define CMD_JEDEC_ID      0x9F

#define STATUS_WIP        0x01   /* bit0: Write In Progress */

#define CS_LOW()   HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET)
#define CS_HIGH()  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET)

static SPI_HandleTypeDef *s_hspi = NULL;   /* 初始化時傳入 */

fl_status_t flash_init(SPI_HandleTypeDef *hspi)
{
    if (hspi == NULL) return FL_ERR_PARAM;
    s_hspi = hspi;
    CS_HIGH();                 /* 確保閒置時 CS 為高 */
    return FL_OK;
}

/* ---- 內部輔助（不對外）---- */
static void send_cmd_addr(uint8_t cmd, uint32_t addr)
{
    uint8_t buf[4] = { cmd,
                       (addr >> 16) & 0xFF,   /* MSB 先送 */
                       (addr >> 8)  & 0xFF,
                        addr        & 0xFF };
    HAL_SPI_Transmit(s_hspi, buf, 4, HAL_MAX_DELAY);
}

static void write_enable(void)
{
    uint8_t cmd = CMD_WRITE_ENABLE;
    CS_LOW();
    HAL_SPI_Transmit(s_hspi, &cmd, 1, HAL_MAX_DELAY);
    CS_HIGH();
}

static fl_status_t wait_ready(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (flash_read_status() & STATUS_WIP) {
        if ((HAL_GetTick() - start) > timeout_ms) return FL_ERR_TIMEOUT;
    }
    return FL_OK;
}

/* ---- 公開 API ---- */
void flash_read_jedec_id(uint8_t *id)
{
    uint8_t tx[4] = { CMD_JEDEC_ID, 0xFF, 0xFF, 0xFF };
    uint8_t rx[4] = { 0 };

    CS_LOW();
    HAL_SPI_TransmitReceive(s_hspi, tx, rx, 4, HAL_MAX_DELAY);
    CS_HIGH();

    id[0] = rx[1];   /* rx[0] 是送命令那一拍收到的垃圾 */
    id[1] = rx[2];
    id[2] = rx[3];
}

uint8_t flash_read_status(void)
{
    uint8_t tx[2] = { CMD_READ_STATUS, 0xFF };
    uint8_t rx[2] = { 0 };

    CS_LOW();
    HAL_SPI_TransmitReceive(s_hspi, tx, rx, 2, HAL_MAX_DELAY);
    CS_HIGH();
    return rx[1];
}

fl_status_t flash_sector_erase(uint32_t addr)
{
    write_enable();
    CS_LOW();
    send_cmd_addr(CMD_SECTOR_ERASE, addr);
    CS_HIGH();
    return wait_ready(2000);          /* erase 慢，逾時給足 */
}

fl_status_t flash_page_program(uint32_t addr, const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || len > FLASH_PAGE_SIZE) return FL_ERR_PARAM;
    /* 不可跨頁：超出的部分會繞回本頁開頭覆蓋 */
    if (((addr % FLASH_PAGE_SIZE) + len) > FLASH_PAGE_SIZE) return FL_ERR_PARAM;

    write_enable();
    CS_LOW();
    send_cmd_addr(CMD_PAGE_PROGRAM, addr);
    HAL_SPI_Transmit(s_hspi, (uint8_t *)data, len, HAL_MAX_DELAY);
    CS_HIGH();
    return wait_ready(100);
}

void flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    CS_LOW();
    send_cmd_addr(CMD_READ_DATA, addr);
    while (len > 0) {
        uint16_t chunk = (len > 0xFFFFu) ? 0xFFFFu : (uint16_t)len;
        HAL_SPI_Receive(s_hspi, buf, chunk, HAL_MAX_DELAY);
        buf += chunk;
        len -= chunk;
    }
    CS_HIGH();
}
