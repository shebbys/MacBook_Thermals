#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
typedef struct { char major, minor, build, reserved[1]; uint16_t release; } V;
typedef struct { uint16_t version, length; uint32_t cpuPLimit, gpuPLimit, memPLimit; } P;
typedef struct { uint32_t dataSize, dataType; char dataAttributes; } KI;
typedef struct { uint32_t key; V vers; P pLimitData; KI keyInfo; char result, status, data8; uint32_t data32; unsigned char bytes[32]; } SMC;
static io_connect_t con;
static uint32_t k2u(const char *s){return (s[0]<<24)|(s[1]<<16)|(s[2]<<8)|s[3];}
static int call(SMC *in, SMC *out){size_t o=sizeof(SMC);return IOConnectCallStructMethod(con,2,in,sizeof(SMC),out,&o);}
int main(){
  io_service_t s=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleSMC"));
  if(IOServiceOpen(s,mach_task_self(),0,&con)){puts("open fail");return 1;}
  SMC in={0},out={0}; in.key=k2u("#KEY"); in.data8=9; call(&in,&out); in.keyInfo=out.keyInfo; in.data8=5; call(&in,&out);
  uint32_t n=(out.bytes[0]<<24)|(out.bytes[1]<<16)|(out.bytes[2]<<8)|out.bytes[3]; printf("keys %u\n",n);
  for(uint32_t i=0;i<n;i++){
    memset(&in,0,sizeof in); in.data8=8; in.data32=i; call(&in,&out); uint32_t key=out.key;
    char ks[5]={key>>24,key>>16,key>>8,key,0};
    memset(&in,0,sizeof in); in.key=key; in.data8=9; call(&in,&out); KI ki=out.keyInfo;
    char ts[5]={ki.dataType>>24,ki.dataType>>16,ki.dataType>>8,ki.dataType,0};
    if(ks[0]!='T' && ks[0]!='F' && ks[0]!='P') continue;
    if(strcmp(ts,"flt ")) continue;
    in.keyInfo=ki; in.data8=5; call(&in,&out); float f; memcpy(&f,out.bytes,4);
    printf("%s %.1f\n",ks,f);
  }
}
