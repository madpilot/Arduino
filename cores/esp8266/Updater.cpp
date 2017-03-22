#include "Updater.h"
#include "Arduino.h"
#include "eboot_command.h"
#include "interrupts.h"

//#define DEBUG_UPDATER Serial

extern "C" {
    #include "c_types.h"
    #include "spi_flash.h"
    #include "user_interface.h"
}

extern "C" uint32_t _SPIFFS_start;

UpdaterClass::UpdaterClass()
: _async(false)
, _error(0)
, _buffer(0)
, _bufferLen(0)
, _size(0)
, _startAddress(0)
, _currentAddress(0)
, _command(U_FLASH)
{
#ifdef VERIFY_SIGNATURE
  _ca_ctx = (CA_CERT_CTX *)malloc(sizeof(CA_CERT_CTX));
#endif
}

void UpdaterClass::_reset() {
  if (_buffer)
    delete[] _buffer;
  _buffer = 0;
  _bufferLen = 0;
  _startAddress = 0;
  _currentAddress = 0;
  _size = 0;
  _command = U_FLASH;
}

bool UpdaterClass::begin(size_t size, int command) {
  if(_size > 0){
#ifdef DEBUG_UPDATER
    DEBUG_UPDATER.println("[begin] already running");
#endif
    return false;
  }

#ifdef DEBUG_UPDATER
  if (command == U_SPIFFS) {
    DEBUG_UPDATER.println("[begin] Update SPIFFS.");
  }
#endif

  if(size == 0) {
    _error = UPDATE_ERROR_SIZE;
#ifdef DEBUG_UPDATER
    printError(DEBUG_UPDATER);
#endif
    return false;
  }

  if(!ESP.checkFlashConfig(false)) {
    _error = UPDATE_ERROR_FLASH_CONFIG;
#ifdef DEBUG_UPDATER
    printError(DEBUG_UPDATER);
#endif
    return false;
  }

  _reset();
  _error = 0;

  wifi_set_sleep_type(NONE_SLEEP_T);

  uint32_t updateStartAddress = 0;
  if (command == U_FLASH) {
    //size of current sketch rounded to a sector
    uint32_t currentSketchSize = (ESP.getSketchSize() + FLASH_SECTOR_SIZE - 1) & (~(FLASH_SECTOR_SIZE - 1));
    //address of the end of the space available for sketch and update
    uint32_t updateEndAddress = (uint32_t)&_SPIFFS_start - 0x40200000;
    //size of the update rounded to a sector
    uint32_t roundedSize = (size + FLASH_SECTOR_SIZE - 1) & (~(FLASH_SECTOR_SIZE - 1));
    //address where we will start writing the update
    updateStartAddress = updateEndAddress - roundedSize;

#ifdef DEBUG_UPDATER
        DEBUG_UPDATER.printf("[begin] roundedSize:       0x%08X (%d)\n", roundedSize, roundedSize);
        DEBUG_UPDATER.printf("[begin] updateEndAddress:  0x%08X (%d)\n", updateEndAddress, updateEndAddress);
        DEBUG_UPDATER.printf("[begin] currentSketchSize: 0x%08X (%d)\n", currentSketchSize, currentSketchSize);
#endif

    //make sure that the size of both sketches is less than the total space (updateEndAddress)
    if(updateStartAddress < currentSketchSize) {
      _error = UPDATE_ERROR_SPACE;
#ifdef DEBUG_UPDATER
      printError(DEBUG_UPDATER);
#endif
      return false;
    }
  }
  else if (command == U_SPIFFS) {
     updateStartAddress = (uint32_t)&_SPIFFS_start - 0x40200000;
  }
  else {
    // unknown command
#ifdef DEBUG_UPDATER
    DEBUG_UPDATER.println("[begin] Unknown update command.");
#endif
    return false;
  }

  //initialize
  _startAddress = updateStartAddress;
  _currentAddress = _startAddress;
  _size = size;
  _buffer = new uint8_t[FLASH_SECTOR_SIZE];
  _command = command;

#ifdef DEBUG_UPDATER
  DEBUG_UPDATER.printf("[begin] _startAddress:     0x%08X (%d)\n", _startAddress, _startAddress);
  DEBUG_UPDATER.printf("[begin] _currentAddress:   0x%08X (%d)\n", _currentAddress, _currentAddress);
  DEBUG_UPDATER.printf("[begin] _size:             0x%08X (%d)\n", _size, _size);
#endif

#ifdef VERIFY_SIGNATURE
  // If this package has been signed correctly, the last uint32 is the size of the signature
  // the second-last uint32 is the size of the certificate
  _signatureLen = (uint32_t)&_size - sizeof(uint32);
  _certificateLen = (uint32_t)&_size - (2 * sizeof(uint32));
  _signatureStartAddress = (uint32_t)&_size - _signatureLen - (2 * sizeof(uint32));
  _certificateStartAddress = _signatureStartAddress - _certificateLen;

#ifdef DEBUG_UPDATER
  DEBUG_UPDATER.printf("[begin] _signatureLen:       0x%08X (%d)\n", _signatureLen, _signatureLen);
  DEBUG_UPDATER.printf("[begin] _certificateLen:  0x%08X (%d)\n", _certificateLen, _certificateLen);
  DEBUG_UPDATER.printf("[begin] _signatureStartAddress: 0x%08X (%d)\n", _signatureStartAddress, _signatureStartAddress);
  DEBUG_UPDATER.printf("[begin] _certificateStartAddress: 0x%08X (%d)\n", _certificateStartAddress, _certificateStartAddress);
#endif
#endif

  _md5.begin();
  return true;
}

bool UpdaterClass::setMD5(const char * expected_md5){
  if(strlen(expected_md5) != 32)
  {
    return false;
  }
  _target_md5 = expected_md5;
  return true;
}

bool UpdaterClass::end(bool evenIfRemaining){
  if(_size == 0){
#ifdef DEBUG_UPDATER
    DEBUG_UPDATER.println("no update");
#endif
    return false;
  }

  if(hasError() || (!isFinished() && !evenIfRemaining)){
#ifdef DEBUG_UPDATER
    DEBUG_UPDATER.printf("premature end: res:%u, pos:%u/%u\n", getError(), progress(), _size);
#endif

    _reset();
    return false;
  }

  if(evenIfRemaining) {
    if(_bufferLen > 0) {
      _writeBuffer();
    }
    _size = progress();
  }

  _md5.calculate();
  if(_target_md5.length()) {
    if(_target_md5 != _md5.toString()){
      _error = UPDATE_ERROR_MD5;
#ifdef DEBUG_UPDATER
      DEBUG_UPDATER.printf("MD5 Failed: expected:%s, calculated:%s\n", _target_md5.c_str(), _md5.toString().c_str());
#endif
      _reset();
      return false;
    }
#ifdef DEBUG_UPDATER
    else DEBUG_UPDATER.printf("MD5 Success: %s\n", _target_md5.c_str());
#endif
  }

  if(!_verifyEnd()) {
#ifdef DEBUG_UPDATER
    printError(DEBUG_UPDATER);
#endif
    _reset();
    return false;
  }

  if (_command == U_FLASH) {
    eboot_command ebcmd;
    ebcmd.action = ACTION_COPY_RAW;
    ebcmd.args[0] = _startAddress;
    ebcmd.args[1] = 0x00000;
    ebcmd.args[2] = _size;
    eboot_command_write(&ebcmd);

#ifdef DEBUG_UPDATER
    DEBUG_UPDATER.printf("Staged: address:0x%08X, size:0x%08X\n", _startAddress, _size);
  }
  else if (_command == U_SPIFFS) {
    DEBUG_UPDATER.printf("SPIFFS: address:0x%08X, size:0x%08X\n", _startAddress, _size);
#endif
  }

  _reset();
  return true;
}

#ifdef VERIFY_SIGNATURE
    int UpdaterClass::addCA(const uint8_t *cert, int *len) {
      // TODO: Allow more than one CA
      int res = x509_new(cert, len, &(_ca_ctx->cert[0]));

#ifdef DEBUG_UPDATER
      if(res == X509_OK) {
        DEBUG_UPDATER.printf("Loaded CA certificate. Common Name: %s\n", _ca_ctx->cert[0]->cert_dn[X509_COMMON_NAME]);
      } else {
        DEBUG_UPDATER.printf("Unable to load CA certificate: %i\n", res);
      }
#endif

      return res;
    }

    bool UpdaterClass::_loadCertificate(X509_CTX *ctx) {
      uint8_t *cert = (uint8_t *)_certificateStartAddress;
      int res = x509_new(cert, (int *)&_certificateLen, &ctx);
      if(res != X509_OK) {
#ifdef DEBUG_UPDATER
        DEBUG_UPDATER.printf("Unable to load developer certificate: %i\n", res);
#endif
        return false;
      }
#ifdef DEBUG_UPDATER
      DEBUG_UPDATER.printf("Loaded developer certificate. Common Name: %s\n", ctx->cert_dn[X509_COMMON_NAME]);
#endif
      return true;
    }

    bool UpdaterClass::_verifyCertificate(X509_CTX *ctx) {
      int constraint;
      int res = x509_verify(_ca_ctx, ctx, &constraint);

#ifdef DEBUG_UPDATER
      if(res == 0) {
        DEBUG_UPDATER.printf("Developer certificate verified\n");
      } else {
        DEBUG_UPDATED.printf("Developer certificate not verified\n");
      }
#endif

      return res == 0;
    }

    bool UpdaterClass::_decryptSignature(X509_CTX *ctx, unsigned char **hash) {
      const uint8_t *sig = (uint8_t *)_signatureStartAddress;

// This should be derived from the hash type
#define MAX_LEN_KEY 512

      unsigned char sig_bytes[MAX_KEY_LEN];
      int len = RSA_decrypt(ctx->rsa_ctx, (const uint8_t*)sig, sig_bytes, MAX_KEY_LEN, 0);

      if(len == -1) {
        return false;
      }

      if(len != (int)_signatureLen) {
        return false;
      }

      (*hash) = sig_bytes + len - SHA256_SIZE;
      return true;
    }

    bool UpdaterClass::_compareHash(unsigned char **hash) {
      unsigned char hash_computed[SHA256_SIZE];

      SHA256_CTX sha256;
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, _buffer, _bufferLen);
      SHA256_Final(hash_computed, &sha256);

      if(memcmp(hash, hash_computed, SHA256_SIZE) == 0) {
        return true;
      } else {
        return false;
      }
    }

    bool UpdaterClass::_verifySignature() {
      X509_CTX ctx;
      if(!_loadCertificate(&ctx)) {
        return false;
      }

      if(!_verifyCertificate(&ctx)) {
        return false;
      }

      unsigned char *hash;
      if(!_decryptSignature(&ctx, &hash)) {
        return false;
      }

      if(_compareHash(&hash)) {
        free(hash);
        return true;
      } else {
        free(hash);
        return false;
      }
    }
#endif

bool UpdaterClass::_writeBuffer(){

  if(!_async) yield();
  bool result = ESP.flashEraseSector(_currentAddress/FLASH_SECTOR_SIZE);
  if(!_async) yield();
  if (result) {
      result = ESP.flashWrite(_currentAddress, (uint32_t*) _buffer, _bufferLen);
  }
  if(!_async) yield();

  if (!result) {
    _error = UPDATE_ERROR_WRITE;
    _currentAddress = (_startAddress + _size);
#ifdef DEBUG_UPDATER
    printError(DEBUG_UPDATER);
#endif
    return false;
  }
  _md5.add(_buffer, _bufferLen);
  _currentAddress += _bufferLen;
  _bufferLen = 0;
  return true;
}

size_t UpdaterClass::write(uint8_t *data, size_t len) {
  if(hasError() || !isRunning())
    return 0;

  if(len > remaining()){
    //len = remaining();
    //fail instead
    _error = UPDATE_ERROR_SPACE;
    return 0;
  }

  size_t left = len;

  while((_bufferLen + left) > FLASH_SECTOR_SIZE) {
    size_t toBuff = FLASH_SECTOR_SIZE - _bufferLen;
    memcpy(_buffer + _bufferLen, data + (len - left), toBuff);
    _bufferLen += toBuff;
    if(!_writeBuffer()){
      return len - left;
    }
    left -= toBuff;
    if(!_async) yield();
  }
  //lets see whats left
  memcpy(_buffer + _bufferLen, data + (len - left), left);
  _bufferLen += left;
  if(_bufferLen == remaining()){
    //we are at the end of the update, so should write what's left to flash
    if(!_writeBuffer()){
      return len - left;
    }
  }
  return len;
}

bool UpdaterClass::_verifyHeader(uint8_t data) {
    if(_command == U_FLASH) {
        // check for valid first magic byte (is always 0xE9)
        if(data != 0xE9) {
            _error = UPDATE_ERROR_MAGIC_BYTE;
            _currentAddress = (_startAddress + _size);
            return false;
        }
        return true;
    } else if(_command == U_SPIFFS) {
        // no check of SPIFFS possible with first byte.
        return true;
    }
    return false;
}

bool UpdaterClass::_verifyEnd() {
    if(_command == U_FLASH) {

        uint8_t buf[4];
        if(!ESP.flashRead(_startAddress, (uint32_t *) &buf[0], 4)) {
            _error = UPDATE_ERROR_READ;
            _currentAddress = (_startAddress);
            return false;
        }

        // check for valid first magic byte
        if(buf[0] != 0xE9) {
            _error = UPDATE_ERROR_MAGIC_BYTE;
            _currentAddress = (_startAddress);
            return false;
        }

        uint32_t bin_flash_size = ESP.magicFlashChipSize((buf[3] & 0xf0) >> 4);

        // check if new bin fits to SPI flash
        if(bin_flash_size > ESP.getFlashChipRealSize()) {
            _error = UPDATE_ERROR_NEW_FLASH_CONFIG;
            _currentAddress = (_startAddress);
            return false;
        }

        return true;
    } else if(_command == U_SPIFFS) {
        // SPIFFS is already over written checks make no sense any more.
        return true;
    }
    return false;
}

size_t UpdaterClass::writeStream(Stream &data) {
    size_t written = 0;
    size_t toRead = 0;
    if(hasError() || !isRunning())
        return 0;

    if(!_verifyHeader(data.peek())) {
#ifdef DEBUG_UPDATER
        printError(DEBUG_UPDATER);
#endif
        _reset();
        return 0;
    }

    while(remaining()) {
        toRead = data.readBytes(_buffer + _bufferLen,  (FLASH_SECTOR_SIZE - _bufferLen));
        if(toRead == 0) { //Timeout
            delay(100);
            toRead = data.readBytes(_buffer + _bufferLen, (FLASH_SECTOR_SIZE - _bufferLen));
            if(toRead == 0) { //Timeout
                _error = UPDATE_ERROR_STREAM;
                _currentAddress = (_startAddress + _size);
#ifdef DEBUG_UPDATER
                printError(DEBUG_UPDATER);
#endif
                _reset();
                return written;
            }
        }
        _bufferLen += toRead;
        if((_bufferLen == remaining() || _bufferLen == FLASH_SECTOR_SIZE) && !_writeBuffer())
            return written;
        written += toRead;
        yield();
    }
    return written;
}

void UpdaterClass::printError(Stream &out){
  out.printf("ERROR[%u]: ", _error);
  if(_error == UPDATE_ERROR_OK){
    out.println("No Error");
  } else if(_error == UPDATE_ERROR_WRITE){
    out.println("Flash Write Failed");
  } else if(_error == UPDATE_ERROR_ERASE){
    out.println("Flash Erase Failed");
  } else if(_error == UPDATE_ERROR_READ){
    out.println("Flash Read Failed");
  } else if(_error == UPDATE_ERROR_SPACE){
    out.println("Not Enough Space");
  } else if(_error == UPDATE_ERROR_SIZE){
    out.println("Bad Size Given");
  } else if(_error == UPDATE_ERROR_STREAM){
    out.println("Stream Read Timeout");
  } else if(_error == UPDATE_ERROR_MD5){
    out.println("MD5 Check Failed");
  } else if(_error == UPDATE_ERROR_FLASH_CONFIG){
    out.printf("Flash config wrong real: %d IDE: %d\n", ESP.getFlashChipRealSize(), ESP.getFlashChipSize());
  } else if(_error == UPDATE_ERROR_NEW_FLASH_CONFIG){
    out.printf("new Flash config wrong real: %d\n", ESP.getFlashChipRealSize());
  } else if(_error == UPDATE_ERROR_MAGIC_BYTE){
    out.println("Magic byte is wrong, not 0xE9");
  } else {
    out.println("UNKNOWN");
  }
}

UpdaterClass Update;
