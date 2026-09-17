// tools/mpq_put.cpp — replace a file inside an MPQ with a local file.
//   mpq_put <archive.mpq> <local-file> <internal-name>
#include <StormLib.h>
#include <cstdio>
int main(int argc,char**argv){
    if(argc<4){std::printf("usage: mpq_put <mpq> <localfile> <internalname>\n");return 2;}
    HANDLE h=nullptr;
    if(!SFileOpenArchive(argv[1],0,0,&h)){std::printf("open err %u\n",SErrGetLastError());return 1;}
    SFileRemoveFile(h,argv[3],0);
    if(!SFileAddFileEx(h,argv[2],argv[3],MPQ_FILE_COMPRESS|MPQ_FILE_REPLACEEXISTING,MPQ_COMPRESSION_ZLIB,MPQ_COMPRESSION_ZLIB)){
        std::printf("add err %u\n",SErrGetLastError());SFileCloseArchive(h);return 1;}
    std::printf("put ok\n");SFileCloseArchive(h);return 0;
}
