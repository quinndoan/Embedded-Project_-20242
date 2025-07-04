#define TRUE  1
#define FALSE 0
#define bool uint8_t

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_spi.h"
#include "diskio.h"
#include "fatfs_sd.h"
#include "ff.h"
#include "string.h"
#include "stm32f4xx_hal_gpio.h"

// Định nghĩa các biến toàn cục liên quan đến FATFS và SD Card
extern SPI_HandleTypeDef hspi1;
#define HSPI_SDCARD &hspi1
#define SD_CS_PORT GPIOC
#define SD_CS_PIN GPIO_PIN_4

char USER_Path[4];   // Đường dẫn ổ đĩa logic cho FatFs (ví dụ: "0:")
FATFS SDFatFs;       // Đối tượng hệ thống file FATFS
FILINFO fileInfo;    // Thông tin file (tên, thuộc tính, kích thước...)
uint8_t sect[512];   // Buffer 512 byte cho 1 sector

uint8_t result;      // Biến lưu kết quả trả về
uint16_t i;
FRESULT res;         // Kết quả trả về của các hàm FatFs
char *fn;            // Con trỏ tới tên file
DIR dir;             // Đối tượng thư mục
extern UART_HandleTypeDef huart1;
extern volatile uint16_t Timer1, Timer2, Timer3, Timer4; // Bộ đếm thời gian 1ms cho timeout

static volatile DSTATUS Stat = STA_NOINIT; // Trạng thái thẻ SD: chưa khởi tạo, không có thẻ, bảo vệ ghi...
static uint8_t CardType;
static uint8_t PowerFlag = 0;              // Cờ nguồn thẻ SD (1: đã bật nguồn, 0: đã tắt)

/********************************************
 * 1. HÀM SPI GIAO TIẾP VỚI THẺ SD
 ********************************************/
// Các hàm này dùng để giao tiếp SPI mức thấp với thẻ SD

// Kéo chân CS (Chip Select) xuống mức thấp để chọn thẻ SD
static void SELECT(void)
{
    HAL_Delay(1);
}

static void DESELECT(void)
{
    HAL_Delay(1);
}

// Gửi 1 byte qua SPI
static void SPI_TxByte(uint8_t data)
{
    // Truyền 1 byte qua SPI, dùng cho lệnh hoặc dữ liệu
    HAL_SPI_Transmit(HSPI_SDCARD, &data, 1, SPI_TIMEOUT);
}

// Gửi 1 buffer qua SPI
static void SPI_TxBuffer(uint8_t *buffer, uint16_t len)
{
    // Truyền nhiều byte liên tiếp qua SPI
    HAL_SPI_Transmit(HSPI_SDCARD, buffer, len, SPI_TIMEOUT);
}

// Nhận 1 byte qua SPI
static uint8_t SPI_RxByte(void)
{
    // Để nhận dữ liệu từ slave, master phải gửi 1 byte "dummy" (0xFF)
    uint8_t dummy = 0xFF, data;
    HAL_SPI_TransmitReceive(HSPI_SDCARD, &dummy, &data, 1, SPI_TIMEOUT);
    return data;
}

// Nhận 1 byte qua SPI và lưu vào con trỏ buff
static void SPI_RxBytePtr(uint8_t *buff)
{
    *buff = SPI_RxByte();
}

/********************************************
 * 2. HÀM QUẢN LÝ NGUỒN & TRẠNG THÁI THẺ SD
 ********************************************/
// Các hàm này dùng để kiểm soát nguồn và trạng thái sẵn sàng của thẻ SD

// Chờ thẻ SD sẵn sàng (trả về 0xFF), timeout nếu quá lâu
static uint8_t SD_ReadyWait(void)
{
    uint8_t res;
    Timer2 = 100; // Timeout 500ms (Timer2 giảm mỗi 5ms hoặc 10ms tuỳ cấu hình timer)
    do {
        res = SPI_RxByte(); // Đọc liên tục cho đến khi nhận được 0xFF (SD sẵn sàng)
    } while ((res != 0xFF) && Timer2);
    return res;
}

// Bật nguồn thẻ SD và đưa về trạng thái idle
static void SD_PowerOn(void)
{
    uint8_t args[6];
    uint32_t cnt = 0x1FFF; // Đếm timeout cho quá trình khởi tạo
    // Gửi nhiều byte 0xFF để "đánh thức" thẻ SD (theo chuẩn SD)
    DESELECT();
    for(int i = 0; i < 10; i++)
    {
        SPI_TxByte(0xFF);
    }
    // Chọn thẻ SD
    SELECT();
    // Gửi lệnh CMD0 (GO_IDLE_STATE) để đưa thẻ về trạng thái idle
    args[0] = CMD0; // Lệnh CMD0
    args[1] = 0;
    args[2] = 0;
    args[3] = 0;
    args[4] = 0;
    args[5] = 0x95; // CRC hợp lệ cho CMD0 (bắt buộc theo chuẩn SD, các lệnh khác CRC có thể bỏ qua ở chế độ SPI)
    SPI_TxBuffer(args, sizeof(args));
    // Chờ phản hồi 0x01 (idle), hoặc timeout
    while ((SPI_RxByte() != 0x01) && cnt)
    {
        cnt--;
    }
    DESELECT();
    SPI_TxByte(0XFF); // Gửi thêm 1 byte dummy
    PowerFlag = 1; // Đánh dấu đã bật nguồn
}

// Tắt nguồn thẻ SD (chỉ set cờ, không thao tác phần cứng)
static void SD_PowerOff(void)
{
    PowerFlag = 0;
}

// Kiểm tra trạng thái nguồn thẻ SD (1: đã bật, 0: đã tắt)
static uint8_t SD_CheckPower(void)
{
    return PowerFlag;
}

/********************************************
 * 3. HÀM TRUYỀN/NHẬN DỮ LIỆU VỚI THẺ SD
 ********************************************/
// Các hàm này dùng để truyền/nhận block dữ liệu giữa MCU và thẻ SD

// Nhận 1 block dữ liệu từ thẻ SD (thường là 512 byte)
// Trả về TRUE nếu thành công, FALSE nếu lỗi hoặc timeout
static uint8_t SD_RxDataBlock(uint8_t *buff, unsigned int len)
{
    uint8_t token;
    Timer1 = 100; // Timeout 200ms
    // Chờ token bắt đầu dữ liệu (0xFE), hoặc timeout
    do {
        token = SPI_RxByte();
    } while((token == 0xFF) && Timer1);
    if(token != 0xFE) return FALSE; // Không nhận được token hợp lệ
    // Nhận dữ liệu
    do {
        SPI_RxBytePtr(buff++);
    } while(len--);
    // Bỏ qua 2 byte CRC
    SPI_RxByte();
    SPI_RxByte();
    return TRUE;
}

// Gửi 1 block dữ liệu tới thẻ SD (chỉ dùng khi _USE_WRITE == 1)
#if _USE_WRITE == 1
static uint8_t SD_TxDataBlock(const uint8_t *buff, uint8_t token)
{
    uint8_t resp;
    uint8_t i = 0;
    // Chờ thẻ sẵn sàng
    if (SD_ReadyWait() != 0xFF) return FALSE;
    // Gửi token bắt đầu ghi
    SPI_TxByte(token);
    // Nếu không phải token STOP, gửi dữ liệu
    if (token != 0xFD)
    {
        Timer3 = 200;
        Timer4 = 200;
        SPI_TxBuffer((uint8_t*)buff, 512); // Gửi 512 byte dữ liệu
        // Bỏ qua 2 byte CRC
        SPI_RxByte();
        SPI_RxByte();
        // Nhận phản hồi từ thẻ SD
        while (i <= 64 && Timer4)
        {
            resp = SPI_RxByte();
            // 0x05: dữ liệu được chấp nhận
            if ((resp & 0x1F) == 0x05) break;
            i++;
        }
        // Xoá bộ đệm nhận
        while (SPI_RxByte() == 0 && Timer3);
    }
    // Kiểm tra phản hồi cuối cùng
    if ((resp & 0x1F) == 0x05) return TRUE;
    return FALSE;
}
#endif /* _USE_WRITE */

// Gửi lệnh tới thẻ SD (SPI CMD)
// cmd: mã lệnh, arg: tham số 32 bit
// Trả về mã phản hồi từ thẻ SD
static uint8_t SD_SendCmd(uint8_t cmd, uint32_t arg)
{
    uint8_t crc, res;
    // Chờ thẻ sẵn sàng
    if (SD_ReadyWait() != 0xFF) return 0xFF;
    // Gửi lệnh và tham số
    SPI_TxByte(cmd); // Command
    SPI_TxByte((uint8_t)(arg >> 24)); // Argument[31..24]			// SPI truyền 8 bit một lần, và cần gửi 32bit theo big endian
    SPI_TxByte((uint8_t)(arg >> 16)); // Argument[23..16]
    SPI_TxByte((uint8_t)(arg >> 8));  // Argument[15..8]
    SPI_TxByte((uint8_t)arg);         // Argument[7..0]
    // CRC chỉ bắt buộc với CMD0 và CMD8, các lệnh khác có thể CRC bất kỳ
    if(cmd == CMD0) crc = 0x95;      // CRC đúng cho CMD0
    else if(cmd == CMD8) crc = 0x87; // CRC đúng cho CMD8
    else crc = 1;                    // CRC bất kỳ
    SPI_TxByte(crc);
    // Nếu là lệnh STOP_TRANSMISSION (CMD12), cần đọc bỏ 1 byte
    if (cmd == CMD12) SPI_RxByte();
    // Nhận phản hồi từ thẻ SD (tối đa 10 lần)
    uint8_t n = 10;
    do {
        res = SPI_RxByte();
    } while ((res & 0x80) && --n);
    return res;
}

/********************************************
 * 4. HÀM GIAO TIẾP VỚI FATFS (DISKIO)
 ********************************************/

/* Khởi tạo thẻ SD - phát hiện loại thẻ và chuẩn bị sử dụng */
DSTATUS SD_disk_initialize(uint8_t drv) 
{
	uint8_t n, type, ocr[4];

	/* Chỉ hỗ trợ drive 0 */
	if(drv) return STA_NOINIT;

	/* Kiểm tra có thẻ không */
	if(Stat & STA_NODISK) return Stat;

	/* Bật nguồn thẻ */
	SD_PowerOn();

	/* Chọn thẻ */
	SELECT();

	/* Khởi tạo biến loại thẻ */
	type = 0;

	/* Gửi CMD0 để reset thẻ về trạng thái idle */
	if (SD_SendCmd(CMD0, 0) == 1)
	{
		/* Timeout 1 giây cho quá trình khởi tạo */
		Timer1 = 100;

		/* Kiểm tra SD V2+ bằng CMD8 */
		if (SD_SendCmd(CMD8, 0x1AA) == 1)
		{
			/* Đọc OCR register (4 bytes) */
			for (n = 0; n < 4; n++)
			{
				ocr[n] = SPI_RxByte();
			}

			/* Kiểm tra voltage 2.7-3.6V và check pattern */
			if (ocr[2] == 0x01 && ocr[3] == 0xAA)
			{
				/* Khởi tạo SD V2+ với ACMD41 (HCS=1 hỗ trợ SDHC) */
				do {
					if (SD_SendCmd(CMD55, 0) <= 1 && SD_SendCmd(CMD41, 1UL << 30) == 0) break;
				} while (Timer1);

				/* Đọc OCR để kiểm tra CCS bit */
				if (Timer1 && SD_SendCmd(CMD58, 0) == 0)
				{
					/* Đọc OCR */
					for (n = 0; n < 4; n++)
					{
						ocr[n] = SPI_RxByte();
					}

					/* Phân loại: SDHC/SDXC (block) hoặc SDSC (byte) */
					type = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;
				}
			}
		}
		else
		{
			/* Phân biệt SD V1 và MMC bằng CMD55 */
			type = (SD_SendCmd(CMD55, 0) <= 1 && SD_SendCmd(CMD41, 0) <= 1) ? CT_SD1 : CT_MMC;

			/* Khởi tạo theo loại thẻ */
			do
			{
				if (type == CT_SD1)
				{
					if (SD_SendCmd(CMD55, 0) <= 1 && SD_SendCmd(CMD41, 0) == 0) break; /* SD V1 dùng ACMD41 */
				}
				else
				{
					if (SD_SendCmd(CMD1, 0) == 0) break; /* MMC dùng CMD1 */
				}

			} while (Timer1);

			/* Thiết lập block size = 512 byte cho SD V1/MMC */
			if (!Timer1 || SD_SendCmd(CMD16, 512) != 0) type = 0;
		}
	}

	/* Lưu loại thẻ */
	CardType = type;

	/* Bỏ chọn thẻ */
	DESELECT();
	SPI_RxByte();

	/* Cập nhật trạng thái */
	if (type)
	{
		Stat &= ~STA_NOINIT; /* Khởi tạo thành công */
	}
	else
	{
		SD_PowerOff(); /* Khởi tạo thất bại */
	}

	return Stat;
}

/* Trả về trạng thái thẻ SD */
DSTATUS SD_disk_status(uint8_t drv) 
{
	if (drv) return STA_NOINIT;
	return Stat;
}

/* Đọc sector từ thẻ SD */
DRESULT SD_disk_read(uint8_t pdrv, uint8_t* buff, uint32_t sector, unsigned int count) 
{
	/* Kiểm tra tham số */
	if (pdrv || !count) return RES_PARERR;

	/* Kiểm tra thẻ đã khởi tạo chưa */
	if (Stat & STA_NOINIT) return RES_NOTRDY;

	/* Chuyển địa chỉ sector thành byte cho SD V1/MMC */
	if (!(CardType & CT_SD2)) sector *= 512;

	SELECT();

	if (count == 1)
	{
		/* Đọc 1 block bằng CMD17 */
		if (SD_SendCmd(CMD17, sector) == 0) {
			if (SD_RxDataBlock(buff, 512)) {
				count = 0; /* Thành công */
			} else {
				count = 1; /* Lỗi đọc data */
			}
		} else {
			count = 1; /* Lỗi gửi lệnh */
		}
	}
	else
	{
		/* Đọc nhiều block bằng CMD18 */
		if (SD_SendCmd(CMD18, sector) == 0)
		{
			do {
				if (!SD_RxDataBlock(buff, 512)) break;
				buff += 512; /* Tăng buffer pointer */
			} while (--count);

			/* Dừng truyền */
			SD_SendCmd(CMD12, 0);
		}
	}

	/* Bỏ chọn thẻ */
	DESELECT();
	SPI_RxByte();

	return count ? RES_ERROR : RES_OK;
}

/* Ghi sector vào thẻ SD */
#if _USE_WRITE == 1
DRESULT SD_disk_write(uint8_t pdrv, const uint8_t* buff, uint32_t sector, unsigned int count) // count là số sector
{
	/* Kiểm tra tham số */
	if (pdrv || !count) return RES_PARERR;

	/* Kiểm tra thẻ đã khởi tạo chưa */
	if (Stat & STA_NOINIT) return RES_NOTRDY;

	/* Kiểm tra write protection */
	if (Stat & STA_PROTECT) return RES_WRPRT;

	/* Chuyển địa chỉ sector thành byte cho SD V1/MMC */
	if (!(CardType & CT_SD2)) sector *= 512;

	SELECT();

	if (count == 1)
	{
		/* Ghi 1 block bằng CMD24 */
		if ((SD_SendCmd(CMD24, sector) == 0) && SD_TxDataBlock(buff, 0xFE))
			count = 0; /* Thành công */
	}
	else
	{
		/* Ghi nhiều block bằng CMD25 */
		if (CardType & CT_SD1)
		{
			SD_SendCmd(CMD55, 0);
			SD_SendCmd(CMD23, count); /* Pre-erase cho SD V1 */
		}

		if (SD_SendCmd(CMD25, sector) == 0)
		{
			do {
				if(!SD_TxDataBlock(buff, 0xFC)) break; /* Multi-block token */
				buff += 512; /* Tăng buffer pointer */
			} while (--count);

			/* Gửi stop token */
			if(!SD_TxDataBlock(0, 0xFD))
			{
				count = 1; /* Lỗi stop */
			}
		}
	}

	/* Bỏ chọn thẻ */
	DESELECT();
	SPI_RxByte();

	return count ? RES_ERROR : RES_OK;
}
#endif

/* ioctl */
DRESULT SD_disk_ioctl(uint8_t drv, uint8_t ctrl, void *buff) // ctrl: dạng lệnh, drv: số ổ đĩa vật lý, buff: vùng đệm nhận kết quả
{
	DRESULT res;
	uint8_t n, csd[16], *ptr = buff;
	uint16_t csize;

	/* pdrv should be 0, since it is SD Card */
	if (drv) return RES_PARERR;
	res = RES_ERROR;

	if (ctrl == CTRL_POWER)
	{
		switch (*ptr)
		{
		case 0:
			SD_PowerOff();		/* Power Off */
			res = RES_OK;
			break;
		case 1:
			SD_PowerOn();		/* Power On */
			res = RES_OK;
			break;
		case 2:
			*(ptr + 1) = SD_CheckPower();
			res = RES_OK;		/* Power Check */
			break;
		default:
			res = RES_PARERR;
		}
	}
	else
	{
		/* no disk */
		if (Stat & STA_NOINIT) return RES_NOTRDY;

		SELECT();

		switch (ctrl)
		{
		case GET_SECTOR_COUNT:
			/* SEND_CSD */
			if ((SD_SendCmd(CMD9, 0) == 0) && SD_RxDataBlock(csd, 16))
			{
				if ((csd[0] >> 6) == 1)					// csd: card specific data
				{
					/* SDC V2 */
					csize = csd[9] + ((WORD) csd[8] << 8) + 1;
					*(DWORD*) buff = (DWORD) csize << 10;
				}
				else
				{
					/* MMC or SDC V1 */
					n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
					csize = (csd[8] >> 6) + ((WORD) csd[7] << 2) + ((WORD) (csd[6] & 3) << 10) + 1;
					*(DWORD*) buff = (DWORD) csize << (n - 9);
				}
				res = RES_OK;
			}
			break;
		case GET_SECTOR_SIZE:
			*(WORD*) buff = 512;
			res = RES_OK;
			break;
		case CTRL_SYNC:
			if (SD_ReadyWait() == 0xFF) res = RES_OK;
			break;
		default:
			res = RES_PARERR;
		}

		DESELECT();
		SPI_RxByte();
	}

	return res;
}

/********************************************
 * 5. HÀM THAO TÁC FILE/THƯ MỤC MỨC CAO
 ********************************************/

/* List- File Function */

void SD_List_File(void){
	if(f_mount(&SDFatFs,(TCHAR const*)USER_Path,0)!=FR_OK)
	{
		Error_Handler();
	}
	else
	{
		strcpy(fileInfo.fname, (char*)sect);
		//fileInfo.fname = (char*)sect;
		fileInfo.fsize = sizeof(sect);
		result = f_opendir(&dir, "/");				// mở thư mục gốc trên thẻ
		//Liet ke danh sach tep tin co trong sd card
		if (result == FR_OK)
		{
			while(1)
			{
				result = f_readdir(&dir, &fileInfo);
				if (result==FR_OK && fileInfo.fname[0])		// kiểm tra đọc và lấy ký tự đầu tiên của file/dir
				{
					fn = fileInfo.fname; // Pointer to the LFN buffer

					if(strlen(fn)) HAL_UART_Transmit(&huart1,(uint8_t*)fn,strlen(fn),0x1000);
					else HAL_UART_Transmit(&huart1,(uint8_t*)fileInfo.fname,strlen((char*)fileInfo.fname),0x1000);
					if(fileInfo.fattrib&AM_DIR)				//nếu attribute là DIR thì in thêm DIR để phân biệt
					{
						HAL_UART_Transmit(&huart1,(uint8_t*)"  [DIR]",7,0x1000);
					}
				}
				else break;
				HAL_UART_Transmit(&huart1,(uint8_t*)"\r\n",2,0x1000);
			}
			f_closedir(&dir);
		}
	}
}
/* Hàm tạo thư mục */

void SD_creatSubDir(char* filename)
{
	if(f_mount(&SDFatFs,(TCHAR const*)USER_Path,0)!=FR_OK)
	{
		Error_Handler();
	}
	else
	{
		res = f_stat(filename,&fileInfo);
		switch(res)
		{
			case FR_OK:
				break;
			case FR_NO_FILE:
				res = f_mkdir(filename);				// tạo thư mục mới nếu chưa có
				if(res != FR_OK) Error_Handler();
				break;
			default:
				Error_Handler();
		}
	}
}

/* Hàm này để hỗ trợ xóa được sub dir, sub dir có thể chứa cả file lẫn folder
*/
FRESULT delete_node (
    TCHAR* path,    /* Path name buffer with the sub-directory to delete */
    UINT sz_buff,   /* Size of path name buffer (items) */
    FILINFO* fno    /* Name read buffer */
)
{
    UINT i, j;
    FRESULT fr;
    DIR dir;


    fr = f_opendir(&dir, path); /* Open the directory */
    if (fr != FR_OK) return fr;

    for (i = 0; path[i]; i++) ; /* Get current path length */
    path[i++] = _T('/');

    for (;;) {
        fr = f_readdir(&dir, fno);  /* Get a directory item */
        if (fr != FR_OK || !fno->fname[0]) break;   /* End of directory? */
        j = 0;
        do {    /* Make a path name */
            if (i + j >= sz_buff) { /* Buffer over flow? */
                fr = 100; break;    /* Fails with 100 when buffer overflow, suppose to debug*/
            }
            path[i + j] = fno->fname[j];			// ghép frame vào cuối path
        } while (fno->fname[j++]);
        if (fno->fattrib & AM_DIR) {    /* Item is a directory */
            fr = delete_node(path, sz_buff, fno);
        } else {                        /* Item is a file */
            fr = f_unlink(path);
        }
        if (fr != FR_OK) break;
    }

    path[--i] = 0;  /* Restore the path name */
    f_closedir(&dir);

    if (fr == FR_OK) fr = f_unlink(path);  /* Delete the empty directory */
    return fr;
}
//-------------------------------------------------- Ham xoa thu muc
void SD_deleteFolder(char* foldername)
{
	TCHAR buff[256];
	if(f_mount(&SDFatFs,(TCHAR const*)USER_Path,0)!=FR_OK)
	{
		Error_Handler();
	}
	else
	{
		char path[50] = {0};
		strcpy(path,"0:");
		strcat(path,foldername);			// nối thêm foldername vào path
		res = f_opendir(&dir, path);
		if (res != FR_OK)
		{
			Error_Handler();
		}
		else
		{
			res = delete_node(path,sizeof(buff),&fileInfo);
			/* Check the result */
			if(res == FR_OK)
			{
				send_uart("The directory and its contents have successfully been deleted\r\n");
			}
			else
			{
				send_uart("Failed to delete the directory\r\n");
			}
		}
	}
}
