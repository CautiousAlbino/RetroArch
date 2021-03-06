#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#ifdef HAVE_OPENGLES
#define RCVOpenGLTextureCacheCreateTextureFromImage CVOpenGLESTextureCacheCreateTextureFromImage
#define RCVOpenGLTextureGetName CVOpenGLESTextureGetName
#define RCVOpenGLTextureCacheFlush CVOpenGLESTextureCacheFlush
#define RCVOpenGLTextureCacheCreate CVOpenGLESTextureCacheCreate
#define RCVOpenGLTextureRef CVOpenGLESTextureRef
#define RCVOpenGLTextureCacheRef CVOpenGLESTextureCacheRef
#if COREVIDEO_USE_EAGLCONTEXT_CLASS_IN_API
#define RCVOpenGLGetCurrentContext() (CVEAGLContext)(g_context)
#else
#define RCVOpenGLGetCurrentContext() (__bridge void *)(g_context)
#endif
#else
#define RCVOpenGLTextureCacheCreateTextureFromImage CVOpenGLTextureCacheCreateTextureFromImage
#define RCVOpenGLTextureGetName CVOpenGLTextureGetName
#define RCVOpenGLTextureCacheFlush CVOpenGLTextureCacheFlush
#define RCVOpenGLTextureCacheCreate CVOpenGLTextureCacheCreate
#define RCVOpenGLTextureRef CVOpenGLTextureRef
#define RCVOpenGLTextureCacheRef CVOpenGLTextureCacheRef
#define RCVOpenGLGetCurrentContext() CGLGetCurrentContext(), CGLGetPixelFormat(CGLGetCurrentContext())
#endif

static AVCaptureSession *_session;
static NSString *_sessionPreset;
RCVOpenGLTextureCacheRef textureCache;
GLuint outputTexture;
static bool newFrame = false;

extern void event_process_camera_frame(void* pixelBufferPtr);

void event_process_camera_frame(void* pixelBufferPtr)
{
    CVReturn ret;
    RCVOpenGLTextureRef renderTexture;
    CVPixelBufferRef pixelBuffer = (CVPixelBufferRef)pixelBufferPtr;
    size_t width  = CVPixelBufferGetWidth(pixelBuffer);
    size_t height = CVPixelBufferGetHeight(pixelBuffer);
    
    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    
    (void)width;
    (void)height;
    
    /*TODO - rewrite all this.
     *
     * create a texture from our render target.
     * textureCache will be what you previously 
     * made with RCVOpenGLTextureCacheCreate.
     */
#ifdef HAVE_OPENGLES
    ret = RCVOpenGLTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
                                                      textureCache, pixelBuffer, NULL, GL_TEXTURE_2D,
                                                      GL_RGBA, (GLsizei)width, (GLsizei)height,
                                                      GL_BGRA, GL_UNSIGNED_BYTE, 0, &renderTexture);
#else
    ret = RCVOpenGLTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
                                                      textureCache, pixelBuffer, 0, &renderTexture);
#endif

    if (!renderTexture || ret)
    {
        RARCH_ERR("[apple_camera]: RCVOpenGLTextureCacheCreateTextureFromImage failed.\n");
        return;
    }
    
    outputTexture = RCVOpenGLTextureGetName(renderTexture);
    glBindTexture(GL_TEXTURE_2D, outputTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
    [[NSNotificationCenter defaultCenter] postNotificationName:@"NewCameraTextureReady" object:nil];
    newFrame = true;
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    RCVOpenGLTextureCacheFlush(textureCache, 0);

    CFRelease(renderTexture);
    CFRelease(pixelBuffer);

    pixelBuffer = 0;
}

- (void)captureOutput:(AVCaptureOutput *)captureOutput
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection
{
    // TODO: Don't post if event queue is full
    CVPixelBufferRef pixelBuffer = (CVPixelBufferRef)CVPixelBufferRetain(CMSampleBufferGetImageBuffer(sampleBuffer));
    event_process_camera_frame(pixelBuffer);
}

/* TODO - add void param to onCameraInit so we can pass g_context. */
- (void) onCameraInit
{
    NSError *error;
    AVCaptureVideoDataOutput * dataOutput;
    AVCaptureDeviceInput *input;
    AVCaptureDevice *videoDevice;

    CVReturn ret = RCVOpenGLTextureCacheCreate(kCFAllocatorDefault, NULL,
    RCVOpenGLGetCurrentContext(), NULL, &textureCache);
    (void)ret;
    
    //-- Setup Capture Session.
    _session = [[AVCaptureSession alloc] init];
    [_session beginConfiguration];
    
    // TODO: dehardcode this based on device capabilities
    _sessionPreset = AVCaptureSessionPreset640x480;
    
    //-- Set preset session size.
    [_session setSessionPreset:_sessionPreset];
    
    //-- Creata a video device and input from that Device.  Add the input to the capture session.
    videoDevice = (AVCaptureDevice*)[AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    if (videoDevice == nil)
        assert(0);
    
    //-- Add the device to the session.
    input = (AVCaptureDeviceInput*)[AVCaptureDeviceInput deviceInputWithDevice:videoDevice error:&error];
    if (error)
    {
        RARCH_ERR("video device input %s\n", error.localizedDescription.UTF8String);
        assert(0);
    }
    
    [_session addInput:input];
    
    /* Create the output for the capture session. */
    dataOutput = (AVCaptureVideoDataOutput*)[[AVCaptureVideoDataOutput alloc] init];
    [dataOutput setAlwaysDiscardsLateVideoFrames:NO]; /* Probably want to set this to NO when recording. */
    
	[dataOutput setVideoSettings:[NSDictionary dictionaryWithObject:[NSNumber numberWithInt:kCVPixelFormatType_32BGRA] forKey:(id)kCVPixelBufferPixelFormatTypeKey]];
    
    /* Set dispatch to be on the main thread so OpenGL can do things with the data. */
    [dataOutput setSampleBufferDelegate:self queue:dispatch_get_main_queue()];
    
    [_session addOutput:dataOutput];
    [_session commitConfiguration];
}

- (void) onCameraStart
{
    [_session startRunning];
}

- (void) onCameraStop
{
    [_session stopRunning];
}

- (void) onCameraFree
{
    RCVOpenGLTextureCacheFlush(textureCache, 0);
    CFRelease(textureCache);
}
