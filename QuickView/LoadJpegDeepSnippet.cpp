
// ============================================================================
// LibJpeg Deep Implementation (Scanline Cancellation)
// ============================================================================
struct my_error_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
};

METHODDEF(void) my_error_exit(j_common_ptr cinfo) {
  my_error_mgr* myerr = (my_error_mgr*)cinfo->err;
  longjmp(myerr->setjmp_buffer, 1);
}

// Low-level decode with scanline cancellation
static HRESULT LoadJpegDeep(const std::vector<uint8_t>& buf, CImageLoader::DecodedImage* pOut, 
                            int targetW, int targetH, 
                            std::wstring* pLoaderName,
                            CImageLoader::CancelPredicate checkCancel) 
{
    struct jpeg_decompress_struct cinfo;
    struct my_error_mgr jerr;
    
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;
    
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return E_FAIL;
    }
    
    jpeg_create_decompress(&cinfo);
    // jpeg_mem_src is a standard extension in libjpeg-turbo / modern jpeg
    jpeg_mem_src(&cinfo, buf.data(), (unsigned long)buf.size());
    
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return E_FAIL;
    }
    
    // IDCT Scaling Logic
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    
    if (targetW > 0 && targetH > 0) {
        // Calculate scale factor (M/8)
        // We start from 1/1 (scale_denom 1) and increase denominator (1, 2, 4, 8)
        // Check if next factor (double denom) is still >= target
        while (cinfo.scale_denom < 8) {
             int nextDenom = cinfo.scale_denom * 2;
             int scaledW = (cinfo.image_width + nextDenom - 1) / nextDenom; // safe div
             int scaledH = (cinfo.image_height + nextDenom - 1) / nextDenom;
             
             if (scaledW < targetW || scaledH < targetH) break; // Too small
             cinfo.scale_denom = nextDenom;
        }
    }
    
    // Force Output Color Space (Direct to BGRA for D2D)
    cinfo.out_color_space = JCS_EXT_BGRA; 
    
    jpeg_start_decompress(&cinfo);
    
    // Output dimensions
    int w = cinfo.output_width;
    int h = cinfo.output_height;
    
    // Allocate PMR
    UINT stride = w * 4;
    try {
        pOut->pixels.resize((size_t)stride * h);
    } catch(...) {
        jpeg_destroy_decompress(&cinfo);
        return E_OUTOFMEMORY;
    }
    
    pOut->width = w;
    pOut->height = h;
    pOut->stride = stride;
    
    // Scanline Loop (Check Cancel!)
    while (cinfo.output_scanline < cinfo.output_height) {
        // [Deep Check]
        if (checkCancel && checkCancel()) {
            jpeg_abort_decompress(&cinfo); // Cleanup internal state
            jpeg_destroy_decompress(&cinfo);
            return E_ABORT;
        }
        
        JSAMPROW row_pointer[1];
        row_pointer[0] = &pOut->pixels[(size_t)cinfo.output_scanline * stride];
        jpeg_read_scanlines(&cinfo, row_pointer, 1);
    }
    
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    
    if (pLoaderName) {
        *pLoaderName = L"FASTJPEG (Deep Cancel)";
        if (cinfo.output_width < cinfo.image_width) *pLoaderName += L" [Scaled]";
    }
    
    pOut->isValid = true;
    return S_OK;
}
