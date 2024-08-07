#include <assert.h>
#include <asm-generic/fcntl.h>
#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define CEGL() { EGLint err = eglGetError(); if(err != EGL_SUCCESS) { printf("EGL error: %d\n", err); assert(0); } }

/*
 * - Upload a bitmap ? texture
 * - Output to a FBO ?
 * - Use dma_buf
 */
typedef struct {
	int width;
	int height;
	int imagesize;
	unsigned char *data;
	char *dma_buf_in;
	char *dma_buf_out;
} textureImage;

/* simple loader for 24bit bitmaps (data is in rgb-format) - NeHe tutorials */
int loadBMP(char *filename, textureImage *texture)
{
	FILE *file;
	unsigned short int bfType;
	uint32_t bfOffBits;
	uint16_t biPlanes;
	uint16_t biBitCount;
	uint32_t biSizeImage;
	int i;
	unsigned char temp;

	/* make sure the file is there and open it read-only (binary) */
	if ((file = fopen(filename, "rb")) == NULL) {
		printf("File not found : %s\n", filename);
		return -1;
	}

	if(!fread(&bfType, 2, 1, file)) {
		printf("Error reading file!\n");
		return -1;
	}

	/* check if file is a bitmap */
	if (bfType != 0x4d42) {
		printf("Not a Bitmap-File!\n");
		return -1;
	}

	/* get the file size */
	/* skip file size and reserved fields of bitmap file header */
	fseek(file, 8, SEEK_CUR);

	/* get the position of the actual bitmap data */
	if (!fread(&bfOffBits, 4, 1, file)) {
		printf("Error reading file!\n");
		return -1;
	}
	printf("Data at Offset: %ld\n", bfOffBits);

	/* skip size of bitmap info header */
	fseek(file, 4, SEEK_CUR);

	/* get the width of the bitmap */
	fread(&texture->width, 4, 1, file);
	printf("Width of Bitmap: %d\n", texture->width);

	/* get the height of the bitmap */
	fread(&texture->height, 4, 1, file);
	printf("Height of Bitmap: %d\n", texture->height);

	/* get the number of planes (must be set to 1) */
	fread(&biPlanes, 2, 1, file);
	if (biPlanes != 1) {
		printf("Error: number of Planes not 1!\n");
		return -1;
	}

	/* get the number of bits per pixel */
	if (!fread(&biBitCount, 2, 1, file)) {
		printf("Error reading file!\n");
		return -1;
	}

	printf("Bits per Pixel: %d\n", biBitCount);
	if (biBitCount != 24) {
		printf("Bits per Pixel not 24\n");
		return -1;
	}

	/* calculate the size of the image in bytes */
	biSizeImage = texture->width * texture->height * 3;
	printf("Size of the image data: %ld\n", biSizeImage);
	texture->data = (unsigned char*)malloc(biSizeImage);

	/* seek to the actual data */
	fseek(file, bfOffBits, SEEK_SET);
	if (!fread(texture->data, biSizeImage, 1, file)) {
		printf("Error loading file!\n");
		return -1;
	}

	/* swap red and blue (bgr -> rgb) */
	for (i = 0; i < biSizeImage; i += 3) {
		temp = texture->data[i];
		texture->data[i] = texture->data[i + 2];
		texture->data[i + 2] = temp;
	}
	texture->imagesize = biSizeImage;

	return 0;
}

static const GLchar* vertex_shader_source =
	"#version 300 es\n"
	"in vec3 position;\n"
	"in vec2 tx_coords;\n"
	"out vec2 v_texCoord;\n"
	"void main() {  \n"
	"       gl_Position = vec4(position, 1.0);\n"
	"       v_texCoord = tx_coords;\n"
	"}\n";
    
static const GLchar* fragment_shader_source =
	"#version 300 es\n"
	"#extension GL_OES_EGL_image_external : require\n"
	"#extension GL_OES_EGL_image_external_essl3 : enable\n"
	"precision mediump float;\n"
	"uniform samplerExternalOES uSampler;\n"
	"in vec2 v_texCoord;\n"
	"out vec4 out_color;\n"
	"void main() {  \n"
	"       out_color = texture( uSampler, v_texCoord );\n"
	"}\n";

static const GLfloat vertices[][4][3] =
{
	{ {-1.0, -1.0, 0.0}, { 1.0, -1.0, 0.0}, {-1.0, 1.0, 0.0}, {1.0, 1.0, 0.0} }
};

static const GLfloat uv_coords[][4][2] =
{
	{ {0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0} }
};

void shader_dump_log(GLint shader, const char *shader_name)
{
	GLint sizeLog = 0;
	GLchar *infoLog;

	glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &sizeLog);
	infoLog = malloc(sizeLog);
	memset(infoLog, 0x0, sizeLog);

	glGetShaderInfoLog(shader, sizeLog, &sizeLog, infoLog);
	fprintf(stderr, "%s program link failed logsize %d log = %s\n", shader_name, sizeLog, infoLog);

	free(infoLog);
}

GLint common_get_shader_program(const char *vertex_shader_source, const char *fragment_shader_source)
{
	enum Consts {INFOLOG_LEN = 512};
	GLchar infoLog[INFOLOG_LEN];
	GLint fragment_shader;
	GLint shader_program;
	GLint success = GL_FALSE;
	GLint vertex_shader;
	GLint length;

	vertex_shader = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
	glCompileShader(vertex_shader);
	glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
	if (success == GL_FALSE) {
		shader_dump_log(vertex_shader, "vertex_shader");
		return success;
	}

	/* Fragment shader */
	fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
	glCompileShader(fragment_shader);
	glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
	if (success == GL_FALSE) {
		shader_dump_log(fragment_shader, "fragment_shader");
		return success;
	}

	/* Link shaders */
	shader_program = glCreateProgram();
	glAttachShader(shader_program, vertex_shader);
	glAttachShader(shader_program, fragment_shader);
	glLinkProgram(shader_program);
	glGetProgramiv(shader_program, GL_LINK_STATUS, &success);

	if (success == GL_FALSE) {
		shader_dump_log(fragment_shader, "program_link");
		return success;
	}

	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);

	return shader_program;
}

int getDmaHeap(textureImage *texImage, const char *alloc_name)
{
	const char *cma = "/dev/dma_heap/linux,cma";
	int ret;
	int fd;

	struct dma_heap_allocation_data heap_data = {
		.len = texImage->imagesize,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};

	fd = open(cma, O_CLOEXEC | O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Open of %s failed\n", cma);
		return fd;
	}

	ret = ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &heap_data);
	if (ret) {
		fprintf(stderr, "DMA_HEAP_IOCTL_ALLOC fail %d\n", ret);
		close(fd);
		return ret;
	}

	ret = ioctl(heap_data.fd, DMA_BUF_SET_NAME, alloc_name);
	if (ret) {
		fprintf(stderr, "DMA_BUF_SET_NAME %s fail %d\n", alloc_name, ret);
		close(fd);
		return ret;
	}

	return heap_data.fd;
}

void cleanup_gbm(int fd, struct gbm_device *gbm)
{
        if (gbm)
                gbm_device_destroy(gbm);

	if (fd >= 0)
        	close(fd);

}

int initEGLContext(EGLDisplay *display, int *fd, struct gbm_device **gbm)
{
	EGLContext context;
        EGLint configAttribs[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_CONFORMANT, EGL_OPENGL_ES2_BIT,
                EGL_NONE
        };
        EGLint contextAttribs[] = {
                EGL_CONTEXT_MAJOR_VERSION, 3,
                EGL_CONTEXT_MINOR_VERSION, 1,
                EGL_NONE
        };
        EGLint numConfigs;
        EGLConfig config;
        EGLint major;
        EGLint minor;
        const char *dri_node = "/dev/dri/renderD128"; //TODO: get from an env or config setting

        *fd = open(dri_node, O_RDWR);
        if (*fd < 0) {
                fprintf(stderr, "Open %s fail %d\n", dri_node, *fd);
                return *fd;
        }

        *gbm = gbm_create_device(*fd);
        if (!*gbm) {
                fprintf(stderr, "gbm_crate_device fail\n");
                goto fail;
        }

        *display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, *gbm, NULL);
        if (*display == EGL_NO_DISPLAY) {
                fprintf(stderr, "Unable to get EGL display\n");
                goto fail;
        }

        if (eglInitialize(*display, &major, &minor) != EGL_TRUE) {
                fprintf(stderr, "eglInitialize fail\n");
                goto fail;
        }

        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
                fprintf(stderr, "API bind fail\n");
                goto fail;
        }

        if (eglChooseConfig(*display, configAttribs, &config, 1, &numConfigs) != EGL_TRUE) {
                fprintf(stderr, "eglChooseConfig fail\n");
                goto fail;
        }

        context = eglCreateContext(*display, config, EGL_NO_CONTEXT, contextAttribs);
        if (context == EGL_NO_CONTEXT) {
                fprintf(stderr, "eglContext returned EGL_NO_CONTEXT\n");
        }

        if (eglMakeCurrent(*display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) != EGL_TRUE) {
                fprintf(stderr, "eglMakeCurrent fail\n");
        }

        return 0;
fail:
	cleanup_gbm(*fd, *gbm);

        return -ENODEV;
}

int main(int argc, char *argv[])
{
	extern int errno;
	int dmaFd = -1;
	int fd = -1;
	struct gbm_device *gbm = NULL;
	textureImage texImage;
	GLuint shader_program, vbo;
	GLint pos;
	GLint uvs;
	const char *exts;
	int ret;
	EGLint err;
	EGLDisplay display;

	struct texture_storage_metadata_t {
		int fourcc;
		EGLuint64KHR modifiers;
		EGLint stride;
		EGLint offset;
	};

        int texture_dmabuf_fd = -1;
        struct texture_storage_metadata_t texture_storage_metadata;
    
        int num_planes;

	if (loadBMP("Data/image2.bmp", &texImage))
		return -1;

	dmaFd = getDmaHeap(&texImage, "libcamera-egl-test");
	if (dmaFd < 0) {
		ret = dmaFd;
		goto done;
	}

	texImage.dma_buf_in = mmap(NULL, texImage.imagesize, PROT_READ | PROT_WRITE , MAP_SHARED, dmaFd, 0);
	if (texImage.dma_buf_in == MAP_FAILED) {
		fprintf(stderr, "Unable to mmap dmabuf errno %d %s\n", errno, strerror(errno));
		goto done;
	}

	memcpy(texImage.dma_buf_in, texImage.data, texImage.imagesize);

	if (initEGLContext(&display, &fd, &gbm))
		goto done;

	exts = eglQueryString(display, EGL_EXTENSIONS);
	if (!exts) {
		fprintf(stderr, "EGL_EXTENSIONS lookup fail\n");
		ret = -ENODEV;
		goto done;
	}

	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
	PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;
	PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA = NULL;
	PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA = NULL;
            
	if (strstr(exts, "EGL_KHR_image") || strstr(exts, "EGL_KHR_image_base")) {
		eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
		eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
		glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
		eglExportDMABUFImageQueryMESA = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)eglGetProcAddress("eglExportDMABUFImageQueryMESA");
		eglExportDMABUFImageMESA = (PFNEGLEXPORTDMABUFIMAGEMESAPROC)eglGetProcAddress("eglExportDMABUFImageMESA");
	} else {
		fprintf(stderr, "Display doesn't support required eGL extenions !\n");
		ret = -ENODEV;
		goto done;
	}

	shader_program = common_get_shader_program(vertex_shader_source, fragment_shader_source);
	pos = glGetAttribLocation(shader_program, "position");
	uvs = glGetAttribLocation(shader_program, "tx_coords");

	EGLImageKHR dma_image;
	dma_image = eglCreateImageKHR(display,
				      EGL_NO_CONTEXT,
				      EGL_LINUX_DMA_BUF_EXT,
				      NULL,
				      (EGLint[]) {
					EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
					EGL_WIDTH, texImage.width,
					EGL_HEIGHT, texImage.height,
					EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,  /// takes 16 or 32 bits per pixel (or 8 probably)
					EGL_DMA_BUF_PLANE0_FD_EXT, dmaFd,
					EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
					EGL_DMA_BUF_PLANE0_PITCH_EXT, texImage.width * 4,
					EGL_NONE, EGL_NONE } );

	if(dma_image == EGL_NO_IMAGE_KHR) { 
		err = eglGetError();
		printf("error: eglCreateImageKHR failed\n");

		switch(err) {
		case EGL_BAD_DISPLAY:
			printf("EGL_BAD_DISPLAY\n");
			break;
		case EGL_BAD_PARAMETER:
			printf("EGL_BAD_PARAMETER\n");
			break;
		case EGL_BAD_MATCH:
			printf("EGL_BAD_MATCH\n");
			break;
		case EGL_BAD_ACCESS:
			printf("EGL_BAD_ACCESS\n");
			break;
		case EGL_BAD_ALLOC:
			printf("EGL_BAD_ALLOC\n");
			break;
		default:
			printf("Unmapped error code %d\n", err);
			break;
		}
		ret = -ENODEV;
		goto done;	
	}

	// Sanity check the created image
	EGLBoolean queried = eglExportDMABUFImageQueryMESA(display,
                                                           dma_image,
                                                           &texture_storage_metadata.fourcc,
                                                           &num_planes,
                                                           &texture_storage_metadata.modifiers);
        assert(queried);
        assert(num_planes == 1);

	EGLBoolean exported = eglExportDMABUFImageMESA(display,
                                                       dma_image,
                                                       &texture_dmabuf_fd,
                                                       &texture_storage_metadata.stride,
                                                       &texture_storage_metadata.offset);
        assert(exported);

        char *fourcc = (char*)&texture_storage_metadata.fourcc;
        printf("texture_storage fourcc 0x%08x = %c%c%c%c planes 0x%08x modifiers 0x%08x stride 0x%08x offset 0x%08x\n",
                texture_storage_metadata.fourcc,
                fourcc[0],
                fourcc[1],
                fourcc[2],
                fourcc[3],
                texture_storage_metadata.modifiers,
                texture_storage_metadata.stride,
                texture_storage_metadata.offset);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glViewport(0, 0, texImage.width, texImage.height);

	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices)+sizeof(uv_coords), 0, GL_STATIC_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
	glBufferSubData(GL_ARRAY_BUFFER, sizeof(vertices), sizeof(uv_coords), uv_coords);
	glEnableVertexAttribArray(pos);
	glEnableVertexAttribArray(uvs);
	glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, 0, (GLvoid*)0);
	glVertexAttribPointer(uvs, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid*)sizeof(vertices)); /// last is offset to loc in buf memory
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	GLuint dma_texture;
	glGenTextures(1, &dma_texture);
	glEnable(GL_TEXTURE_EXTERNAL_OES);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, dma_texture);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, dma_image);
	glGetUniformLocation(shader_program, "texture");

	ret = 0;

	glClearColor(1.0f, 0.0f, 0.0f, 1.0f);

	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, dma_image); CEGL();
	glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); CEGL();
	glUseProgram(shader_program); CEGL();
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); CEGL();

#if 0
	eglSwapBuffers(display, EGL_NO_SURFACE); CEGL();
#endif
        int j = 0, i = 0;
#if 0
	char * pixels = malloc(texImage.width* texImage.width * 4);
	glReadPixels(0, 0, texImage.width, texImage.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        printf("Pixels: ");
        for (;  i < texImage.width * texImage.height * 4; i++) {
            printf("%x ", pixels[i]);
            if (++j == 4) {
                j = 0;
                printf("\n");
            }
        }
	free(pixels);
#endif
	texImage.dma_buf_out = mmap(NULL, texImage.imagesize, PROT_READ , MAP_SHARED, texture_dmabuf_fd, 0);
	if (texImage.dma_buf_out == MAP_FAILED) {
		fprintf(stderr, "Unable to mmap dmabuf errno %d %s\n", errno, strerror(errno));
		goto done;
	}

        printf("DMA Pixels: ");
        j = 0;
        i = 0;
        for (;  i < texImage.width * texImage.height * 4; i++) {
            printf("%x ", texImage.dma_buf_out[i]);
            if (++j == 4) {
                j = 0;
                printf("\n");
            }
        }

done:
	if (texImage.dma_buf_out != MAP_FAILED)
		munmap(texImage.dma_buf_out, texImage.imagesize);
	if (texture_dmabuf_fd > 0)
		close(texture_dmabuf_fd);
	eglDestroyImageKHR(display, dma_image);
	free(texImage.data);
	cleanup_gbm(fd, gbm);
	if (texImage.dma_buf_in != MAP_FAILED)
		munmap(texImage.dma_buf_in, texImage.imagesize);
	if (dmaFd > 0)
		close(dmaFd);

	return ret;
}
