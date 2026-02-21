#import <Metal/Metal.h>

namespace omegasl {

    void compileMTLShader(void *mtl_device,unsigned length,const char *string,void **pDest){
        id<MTLDevice> device = (__bridge id)mtl_device;
        NSError *error;
        MTLCompileOptions *compileOptions = [[MTLCompileOptions alloc] init];

        id<MTLLibrary> library = [device newLibraryWithSource:[[NSString alloc] initWithBytesNoCopy:(void *)string length:length encoding:NSUTF8StringEncoding freeWhenDone:NO] options:compileOptions error:&error];
        if(library == nil){
            NSLog(@"OmegaSL Metal compile failed: %@",error);
        }
        if(library != nil){
            CFRetain((__bridge CFTypeRef)library);
        }
        *pDest = (__bridge void *)library;
    }

}
