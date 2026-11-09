// Original NBT x86 ABI, checked against this vehicle's CoreFramework library.
// These declarations are not the newer Evo ABI.
#ifndef NBT_IMAGE_ABI_HPP
#define NBT_IMAGE_ABI_HPP
#include <stdint.h>
namespace bmw {
template<class T> class BasicString {
public:
    BasicString(const T*);
    BasicString(const BasicString&);
    ~BasicString();
private:
    T* data_;
};
class BufferStream {
public:
    ~BufferStream();
    const void* getBuffer() const;
    bool read2(void*, unsigned, unsigned*) const;
private:
    uint32_t storage_[6];
};
class Image {
public:
    Image(const unsigned char*, unsigned);
    Image(const Image&);
    const BufferStream& getImageStream() const;
    // Implicit destructor destroys BufferStream then BasicString, in that order.
private:
    BasicString<char> filename_; // +0; _basicStringInitData, not a vtable!
    BufferStream stream_;       // +4, native BufferStream destructor owns allocation
    bool loaded_;               // +28
};
class Runnable {
public:
    virtual ~Runnable() {}
    virtual void run() = 0;
};
}
typedef char NbtImageSize[(sizeof(bmw::Image)==32)?1:-1];
typedef char NbtBufferSize[(sizeof(bmw::BufferStream)==24)?1:-1];
#endif
