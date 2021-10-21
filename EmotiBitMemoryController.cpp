#include "EmotiBitMemoryController.h"

bool EmotiBitMemoryController::init(TwoWire &emotibit_i2c, EmotiBitVersionController::EmotiBitVersion version)
{
	_version = version;
	if (_version == EmotiBitVersionController::EmotiBitVersion::V04A)
	{
		if (emotibitEeprom.begin(EMOTIBIT_EEPROM_I2C_ADDRESS, emotibit_i2c))
		{
			emotibitEepromSettings.capacityBytes = 256; // in bytes
			emotibitEepromSettings.pageSizeBytes = 16; // in bytes
			emotibitEeprom.setMemorySize(emotibitEepromSettings.capacityBytes);
			emotibitEeprom.setPageSize(emotibitEepromSettings.pageSizeBytes);
			return true;
		}
		else // Flash module failed to init. check i2c
		{
			return false;
		}
	}
	else if ((int)_version <= (int)EmotiBitVersionController::EmotiBitVersion::V04A)
	{
		if (si7013.setup(emotibit_i2c))
		{
			return true;
		}
		else // could not comm. with IC. check i2c line
		{
			return false;
		}
	}
}

uint8_t EmotiBitMemoryController::requestToWrite(DataType datatype, uint8_t version, size_t size, uint8_t *data, bool syncWrite)
{
	if (_version == EmotiBitVersionController::EmotiBitVersion::V04A)
	{
		if (data != nullptr)
		{
			if (datatype != DataType::length)
			{
				updateBuffer(datatype, version, size, data);
			}
			else
			{
				return (uint8_t)Error::OUT_OF_BOUNDS_ACCESS;
			}
		}
		else
		{
			return (uint8_t)Error::FAILURE;
		}

		if (datatype == DataType::VARIANT_INFO)
		{
			uint8_t status;
			// write the updated buffer to flash
			status = writeToEeprom();
			return status;
		}
		else
		{
			if (syncWrite)
			{
				status = MemoryControllerStatus::BUSY;
				// ToDo: plan to make it asynchronous
				// wait till data is written in the ISR
				while (status == MemoryControllerStatus::BUSY);
				return (uint8_t)writeResult;  // returns SUCCESS if write complete
			}
			else
			{
				return 0; // no error
			}
		}
	}
}

void EmotiBitMemoryController::updateBuffer(DataType datatype, uint8_t version, size_t size, uint8_t* data)
{
	updateMemoryMap(datatype, size + 1); // the version information requires an additoinal byte
	_buffer.datatype = datatype;
	_buffer.dataTypeVersion = version;
	_buffer.dataLength = size;
	// update in the end bacause ISR can hit anytime
	_buffer.data = data;
}

void EmotiBitMemoryController::Buffer::clear()
{
	data = nullptr;
	dataLength = 0;
	dataTypeVersion = 0;
	datatype = DataType::length;
}

void EmotiBitMemoryController::updateMemoryMap(DataType datatype, size_t size)
{
	map[(uint8_t)datatype].address = _nextAvailableAddress;
	map[(uint8_t)datatype].size = size;
	_nextAvailableAddress += size;
}

uint8_t EmotiBitMemoryController::writeToEeprom()
{
	if (_version == EmotiBitVersionController::EmotiBitVersion::V04A)
	{
		// detect if we have something to write
		if (_buffer.data != nullptr && _buffer.dataLength > 0)
		{
			_numMapSegments++;
			uint8_t i2cWriteStatus = 0;
			emotibitEeprom.write(ConstEepromAddr::NUM_MAP_SEGMENTS, _numMapSegments);

			// write the updated Map
			size_t offsetMapAddress;
			offsetMapAddress = ConstEepromAddr::MEMORY_MAP_BASE + (int)_buffer.datatype * sizeof(EepromMemoryMap);
			uint8_t* mapData;
			mapData = (uint8_t*)(map + ((int)_buffer.datatype)*sizeof(EepromMemoryMap));
			emotibitEeprom.write(offsetMapAddress, mapData, sizeof(   EepromMemoryMap));

			// write the buffer data
			emotibitEeprom.write(map[(int)_buffer.datatype].address, _buffer.data, _buffer.dataLength);
			emotibitEeprom.write(map[(int)_buffer.datatype].address+_buffer.dataLength, _buffer.dataTypeVersion);
		}
		_buffer.clear();
		return 0;
	}
}

uint8_t EmotiBitMemoryController::loadMemoryMap()
{
	if (_version == EmotiBitVersionController::EmotiBitVersion::V04A)
	{
		_numMapSegments = emotibitEeprom.read(ConstEepromAddr::NUM_MAP_SEGMENTS);
		if (_numMapSegments == 255)
		{
			// EEPROM not written
			return (uint8_t)Error::MEMORY_NOT_UPDATED;
		}
		EepromMemoryMap *mapPtr;
		uint8_t *mapData = new uint8_t[sizeof(EepromMemoryMap)*_numMapSegments];
		emotibitEeprom.read(ConstEepromAddr::MEMORY_MAP_BASE, mapData, sizeof(EepromMemoryMap)*_numMapSegments);
		mapPtr = (EepromMemoryMap*)mapData;
		for (uint8_t i = 0; i < _numMapSegments; i++)
		{
			map[i].address = mapPtr->address;
			map[i].size = mapPtr->size;
			mapPtr++;
		}
		return 0;
	}
}

uint8_t EmotiBitMemoryController::readFromMemory(DataType datatype, uint8_t** data)
{
	if (_version == EmotiBitVersionController::EmotiBitVersion::V04A)
	{
		if (map[(uint8_t)datatype].size != 0)
		{
			if (datatype != DataType::length)
			{
				uint8_t *eepromData = new uint8_t[map[(uint8_t)datatype].size];
				if (_version == EmotiBitVersionController::EmotiBitVersion::V04A)
				{
					emotibitEeprom.read(map[(uint8_t)datatype].address, eepromData, map[(uint8_t)datatype].size);
					*data = eepromData;
					return 0;
				}
			}
			else
			{
				return (uint8_t)Error::OUT_OF_BOUNDS_ACCESS;
			}
		}
		else
		{
			return (uint8_t)Error::MEMORY_NOT_UPDATED;
		}
	}
	else if ((int)_version < (int)EmotiBitVersionController::EmotiBitVersion::V04A)
	{
		if (datatype == DataType::VARIANT_INFO)
		{
			uint8_t* otpData = new uint8_t;
			*otpData = si7013.readRegister8(Si7013OtpMemoryMap::EMOTIBIT_VERSION_ADDR, true);
			*data = otpData;
			return 0; // success
		}
		else if (datatype == DataType::EDA)
		{
			uint8_t* otpData = new uint8_t[Si7013OtpMemoryMap::EDA_DATA_SIZE];
			uint8_t *index;
			index = otpData;
			for (uint8_t addr = Si7013OtpMemoryMap::EDA_DATA_BASE_ADDR; addr <= Si7013OtpMemoryMap::EDA_DATA_END_ADDR; addr++)
			{
				 *index = si7013.readRegister8(addr, true);
				 Serial.print("Addr: 0x"); Serial.print(addr, HEX); Serial.print("\t Value:"); Serial.println(*index);
				 index++;
			}
			Serial.print("Location pointed by new func call: 0x"); Serial.println((uint32_t)otpData,HEX);
			*data = otpData;
			return 0; // success
		}
		else
		{
			return (uint8_t)Error::FAILURE;
		}
	}
}