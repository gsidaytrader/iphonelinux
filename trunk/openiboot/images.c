#include "openiboot.h"
#include "images.h"
#include "nor.h"
#include "util.h"
#include "aes.h"
#include "sha1.h"

static const uint32_t NOREnd = 0xF0000;

Image* imageList = NULL;

static uint32_t MaxOffset = 0;
static uint32_t ImagesStart = 0;
static uint32_t SegmentSize = 0;

static const uint8_t Img2HashPadding[] = {	0xAD, 0x2E, 0xE3, 0x8D, 0x2D, 0x9B, 0xE4, 0x35, 0x99, 4,
						0x44, 0x33, 0x65, 0x3D, 0xF0, 0x74, 0x98, 0xD8, 0x56, 0x3B,
						0x4F, 0xF9, 0x6A, 0x55, 0x45, 0xCE, 0x82, 0xF2, 0x9A, 0x5A,
						0xC2, 0xBC, 0x47, 0x61, 0x6D, 0x65, 0x4F, 0x76, 0x65, 0x72,
						0xA6, 0xA0, 0x99, 0x13};

static int IsImg3 = FALSE;

static void calculateHash(Img2Header* header, uint8_t* hash);
static void calculateDataHash(void* buffer, int len, uint8_t* hash);

static int img3_setup() {
	Image* curImage = NULL;
	AppleImg3RootHeader* rootHeader = (AppleImg3RootHeader*) malloc(sizeof(AppleImg3RootHeader));

	uint32_t offset = ImagesStart;
	uint32_t index = 0;
	while(offset < NOREnd) {
		nor_read(rootHeader, offset, sizeof(AppleImg3RootHeader));
		if(rootHeader->base.magic != IMG3_MAGIC)
			break;
		
		if(curImage == NULL) {
			curImage = (Image*) malloc(sizeof(Image));
			imageList = curImage;
		} else {
			curImage->next = (Image*) malloc(sizeof(Image));
			curImage = curImage->next;
		}

		curImage->type = rootHeader->extra.name;
		curImage->offset = offset;
		curImage->length = rootHeader->base.dataSize;
		curImage->padded = rootHeader->base.size;
		curImage->index = index++;
		curImage->hashMatch = TRUE;

		curImage->next = NULL;

		if((offset + curImage->padded) > MaxOffset) {
			MaxOffset = offset + curImage->padded;
		}

		offset += curImage->padded;
	}

	free(rootHeader);

	return 0;
}

int images_setup() {
	IMG2* header;
	Img2Header* curImg2;
	uint8_t hash[0x20];

	MaxOffset = 0;

	header = (IMG2*) malloc(sizeof(IMG2));

	uint32_t IMG2Offset = 0x0;
	for(IMG2Offset = 0; IMG2Offset < NOREnd; IMG2Offset += 4096) {
		nor_read(header, IMG2Offset, sizeof(IMG2));
		if(header->signature == IMG2Signature) {
			break;
		}
	}

	SegmentSize = header->segmentSize;
	ImagesStart = (header->imagesStart + header->dataStart) * SegmentSize;

	AppleImg3Header* img3Header = (AppleImg3Header*) malloc(sizeof(AppleImg3Header));
	nor_read(img3Header, ImagesStart, sizeof(AppleImg3Header));
	if(img3Header->magic == IMG3_MAGIC) {
		img3_setup();
		free(img3Header);
		free(header);
		IsImg3 = TRUE;
		return 0;
	} else {
		free(img3Header);
		IsImg3 = FALSE;
	}

	curImg2 = (Img2Header*) malloc(sizeof(Img2Header));

	Image* curImage = NULL;

	uint32_t curOffset;
	for(curOffset = ImagesStart; curOffset < NOREnd; curOffset += SegmentSize) {
		nor_read(curImg2, curOffset, sizeof(Img2Header));
		if(curImg2->signature != Img2Signature)
			continue;

		uint32_t checksum = 0;
		crc32(&checksum, curImg2, 0x64);

		if(checksum != curImg2->header_checksum) {
			bufferPrintf("mismatch checksum at %x\r\n", curOffset);
			continue;
		}

		if(curImage == NULL) {
			curImage = (Image*) malloc(sizeof(Image));
			imageList = curImage;
		} else {
			curImage->next = (Image*) malloc(sizeof(Image));
			curImage = curImage->next;
		}

		curImage->type = curImg2->imageType;
		curImage->offset = curOffset;
		curImage->length = curImg2->dataLen;
		curImage->padded = curImg2->dataLenPadded;
		curImage->index = curImg2->index;

		memcpy(curImage->dataHash, curImg2->dataHash, 0x40);

		calculateHash(curImg2, hash);

		if(memcmp(hash, curImg2->hash, 0x20) == 0) {
			curImage->hashMatch = TRUE;
		} else {
			curImage->hashMatch = FALSE;
		}

		curImage->next = NULL;

		if((curOffset + curImage->padded) > MaxOffset) {
			MaxOffset = curOffset + curImage->padded;
		}
	}

	free(curImg2);
	free(header);
	
	return 0;
}

void images_list() {
	Image* curImage = imageList;

	while(curImage != NULL) {
		print_fourcc(curImage->type);
		bufferPrintf("(%d/%d): offset: 0x%x, length: 0x%x, padded: 0x%x\r\n", curImage->index, curImage->hashMatch, curImage->offset, curImage->length, curImage->padded);
		curImage = curImage->next;
	}
}

Image* images_get(uint32_t type) {
	Image* curImage = imageList;

	while(curImage != NULL) {
		if(type == curImage->type) {
			return curImage;
		}
		curImage = curImage->next;
	}

	return NULL;
}

void images_release() {
	Image* curImage = imageList;
	Image* toRelease = NULL;
	while(curImage != NULL) {
		toRelease = curImage;
		curImage = curImage->next;
		free(toRelease);
	}
	imageList = NULL;
}

void images_duplicate(Image* image, uint32_t type, int index) {
	if(image == NULL)
		return;

	uint32_t offset = MaxOffset + (SegmentSize - (MaxOffset % SegmentSize));

	uint32_t totalLen = sizeof(Img2Header) + image->padded;
	uint8_t* buffer = (uint8_t*) malloc(totalLen);

	nor_read(buffer, image->offset, totalLen);
	Img2Header* header = (Img2Header*) buffer;
	header->imageType = type;

	if(index >= 0)
		header->index = index;

	calculateDataHash(buffer + sizeof(Img2Header), image->padded, header->dataHash);

	uint32_t checksum = 0;
	crc32(&checksum, buffer, 0x64);
	header->header_checksum = checksum;

	calculateHash(header, header->hash);

	nor_write(buffer, offset, totalLen);

	free(buffer);

	images_release();
	images_setup();
}

void images_duplicate_at(Image* image, uint32_t type, int index, int offset) {
	if(image == NULL)
		return;

	uint32_t totalLen = sizeof(Img2Header) + image->padded;
	uint8_t* buffer = (uint8_t*) malloc(totalLen);

	nor_read(buffer, image->offset, totalLen);
	Img2Header* header = (Img2Header*) buffer;
	header->imageType = type;

	if(index >= 0)
		header->index = index;

	calculateDataHash(buffer + sizeof(Img2Header), image->padded, header->dataHash);

	uint32_t checksum = 0;
	crc32(&checksum, buffer, 0x64);
	header->header_checksum = checksum;

	calculateHash(header, header->hash);

	nor_write(buffer, offset, totalLen);

	free(buffer);

	images_release();
	images_setup();
}

void images_from_template(Image* image, uint32_t type, int index, void* dataBuffer, unsigned int len, int encrypt) {
	if(image == NULL)
		return;

	uint32_t offset = MaxOffset + (SegmentSize - (MaxOffset % SegmentSize));
	uint32_t padded = len;
	if((len & 0xF) != 0) {
		padded = (padded & ~0xF) + 0x10;
	}

	uint32_t totalLen = sizeof(Img2Header) + padded;
	uint8_t* buffer = (uint8_t*) malloc(totalLen);

	nor_read(buffer, image->offset, sizeof(Img2Header));
	Img2Header* header = (Img2Header*) buffer;
	header->imageType = type;

	if(index >= 0)
		header->index = index;

	header->dataLen = len;
	header->dataLenPadded = padded;

	memcpy(buffer + sizeof(Img2Header), dataBuffer, len);
	if(encrypt)
		aes_838_encrypt(buffer + sizeof(Img2Header), padded, NULL);

	calculateDataHash(buffer + sizeof(Img2Header), image->padded, header->dataHash);

	uint32_t checksum = 0;
	crc32(&checksum, buffer, 0x64);
	header->header_checksum = checksum;

	calculateHash(header, header->hash);

	nor_write(buffer, offset, totalLen);

	free(buffer);

	images_release();
	images_setup();
}


void images_erase(Image* image) {
	if(image == NULL)
		return;

	nor_erase_sector(image->offset);

	images_release();
	images_setup();
}

void images_write(Image* image, void* data, unsigned int length, int encrypt) {
	bufferPrintf("images_write(%x, %x, %x)\r\n", image, data, length);
	if(image == NULL)
		return;

	uint32_t padded = length;
	if((length & 0xF) != 0) {
		padded = (padded & ~0xF) + 0x10;
	}

	if(image->next != NULL && (image->offset + sizeof(Img2Header) + padded) >= image->next->offset) {
		bufferPrintf("requested length greater than space available in the hole\r\n");
		return;
	}

	uint32_t totalLen = sizeof(Img2Header) + padded;
	uint8_t* writeBuffer = (uint8_t*) malloc(totalLen);

	nor_read(writeBuffer, image->offset, sizeof(Img2Header));

	memcpy(writeBuffer + sizeof(Img2Header), data, length);

	if(encrypt)
		aes_838_encrypt(writeBuffer + sizeof(Img2Header), padded, NULL);

	Img2Header* header = (Img2Header*) writeBuffer;
	header->dataLen = length;
	header->dataLenPadded = padded;

	calculateDataHash(writeBuffer + sizeof(Img2Header), padded, header->dataHash);

	uint32_t checksum = 0;
	crc32(&checksum, writeBuffer, 0x64);
	header->header_checksum = checksum;

	calculateHash(header, header->hash);

	bufferPrintf("nor_write(%x, %x, %x)\r\n", writeBuffer, image->offset, totalLen);

	nor_write(writeBuffer, image->offset, totalLen);

	bufferPrintf("nor_write(%x, %x, %x) done\r\n", writeBuffer, image->offset, totalLen);

	free(writeBuffer);

	images_release();
	images_setup();

}

unsigned int images_read(Image* image, void** data) {
	if(image == NULL) {
		*data = NULL;
		return 0;
	}

	*data = malloc(image->padded);
	if(!IsImg3) {
		nor_read(*data, image->offset + sizeof(Img2Header), image->length);
		aes_838_decrypt(*data, image->length, NULL);
		return image->length;
	} else {
		nor_read(*data, image->offset, image->padded);

		uint32_t dataOffset = 0;
		uint32_t dataLength = 0;
		uint32_t kbagOffset = 0;
		uint32_t kbagLength = 0;
		uint32_t offset = (uint32_t)(*data + sizeof(AppleImg3RootHeader));
		while((offset - (uint32_t)(*data + sizeof(AppleImg3RootHeader))) < image->length) {
			AppleImg3Header* header = (AppleImg3Header*) offset;
			if(header->magic == IMG3_DATA_MAGIC) {
				dataOffset = offset + sizeof(AppleImg3Header);
				dataLength = header->dataSize;
			}
			if(header->magic == IMG3_KBAG_MAGIC) {
				kbagOffset = offset + sizeof(AppleImg3Header);
				kbagLength = header->dataSize;
			}
			offset += header->size;
		}

		AppleImg3KBAGHeader* kbag = (AppleImg3KBAGHeader*) kbagOffset;

		if(kbag->key_modifier == 1) {
			aes_decrypt((void*)(kbagOffset + sizeof(AppleImg3KBAGHeader)), 16 + (kbag->key_bits * 8), AESGID, NULL, NULL);
		}

		aes_decrypt((void*)dataOffset, (dataLength / 16) * 16, AESCustom, (uint8_t*)(kbagOffset + sizeof(AppleImg3KBAGHeader) + 16), (uint8_t*)(kbagOffset + sizeof(AppleImg3KBAGHeader)));

		uint8_t* newBuf = malloc(dataLength);
		memcpy(newBuf, (void*)dataOffset, dataLength);
		free(*data);
		*data = newBuf;

		return dataLength;
	}
}

static void calculateHash(Img2Header* header, uint8_t* hash) {
	SHA1_CTX context;
	SHA1Init(&context);
	SHA1Update(&context, (uint8_t*) header, 0x3E0);
	SHA1Final(hash, &context);
	memcpy(hash + 20, Img2HashPadding, 32 - 20);
	aes_img2verify_encrypt(hash, 32, NULL);
}

static void calculateDataHash(void* buffer, int len, uint8_t* hash) {
	SHA1_CTX context;
	SHA1Init(&context);
	SHA1Update(&context, buffer, len);
	SHA1Final(hash, &context);
	memcpy(hash + 20, Img2HashPadding, 64 - 20);
	aes_img2verify_encrypt(hash, 64, NULL);
}

int images_verify(Image* image) {
	uint8_t hash[0x40];
	int retVal = 0;

	if(image == NULL) {
		return 1;
	}

	if(!image->hashMatch)
		retVal |= 1 << 2;

	void* data = malloc(image->padded);
	nor_read(data, image->offset + sizeof(Img2Header), image->padded);
	calculateDataHash(data, image->padded, hash);
	free(data);

	if(memcmp(hash, image->dataHash, 0x40) != 0)
		retVal |= 1 << 3;

	return retVal;
}

