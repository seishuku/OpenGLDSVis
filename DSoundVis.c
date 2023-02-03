#include <windows.h>
#include <dsound.h>
#include <gl/gl.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include "overlay.h"
#include "font.h"
#include "fft.h"

#ifdef __GNUC__
#ifndef __int64
#define __int64 long long
#endif
#endif

LPDIRECTSOUNDCAPTURE lpDSCapture=NULL;
LPDIRECTSOUNDCAPTUREBUFFER lpDSBCapture=NULL;

HWND hWnd=NULL;
HDC hDC=NULL;
HGLRC hRC=NULL;

char *szAppName="Realtime Spectrum Analizer - Build: " __DATE__ " " __TIME__
#ifdef _DEBUG
" DEBUG";
#else
" RELEASE";
#endif

// WARNING: The frequency scale is hardcoded to this window width
int Width=1024, Height=768;

int Done=0, Key[256], Active=1;

// FFT resolution
#define size 8192

// Audio sample size
// 256 = approx 5/1000ths of a second time?
#define audio_size 256

const float delta=2.0f/size;

Complex_t left[size], right[size];
float lpeaks[size], rpeaks[size];

unsigned __int64 Frequency, StartTime, EndTime;
float fTimeStep, fTime=0.0f, fTimeFPS=0.0f, FPS;
int Frames=0;

const float fSoundUpdate=1.0f/60.0f;

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void Render(void);
BOOL Init(void);
BOOL CreateDirectSound(void);
void DestroyDirectSound(void);
BOOL CreateOpenGL(void);
void DestroyOpenGL(void);
void Resize(int x, int y, int Width, int Height);

unsigned __int64 rdtsc(void)
{
	return __rdtsc();
}

unsigned __int64 GetFrequency(void)
{
	unsigned __int64 TimeStart, TimeStop, TimeFreq;
	unsigned __int64 StartTicks, StopTicks;
	volatile unsigned __int64 i;

	QueryPerformanceFrequency((LARGE_INTEGER *)&TimeFreq);

	QueryPerformanceCounter((LARGE_INTEGER *)&TimeStart);
	StartTicks=rdtsc();

	for(i=0;i<1000000;i++);

	StopTicks=rdtsc();
	QueryPerformanceCounter((LARGE_INTEGER *)&TimeStop);

	return (StopTicks-StartTicks)*TimeFreq/(TimeStop-TimeStart);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int iCmdShow)
{
	WNDCLASS wc;
	MSG msg;
	RECT Rect;

	wc.style=CS_VREDRAW|CS_HREDRAW|CS_OWNDC;
	wc.lpfnWndProc=WndProc;
	wc.cbClsExtra=0;
	wc.cbWndExtra=0;
	wc.hInstance=hInstance;
	wc.hIcon=LoadIcon(NULL, IDI_WINLOGO);
	wc.hCursor=LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszMenuName=NULL;
	wc.lpszClassName=szAppName;

	RegisterClass(&wc);

	SetRect(&Rect, 0, 0, Width, Height);
	AdjustWindowRect(&Rect, WS_OVERLAPPED|WS_SYSMENU|WS_CAPTION|WS_MINIMIZEBOX, FALSE);

	hWnd=CreateWindow(szAppName, szAppName, WS_OVERLAPPED|WS_SYSMENU|WS_CAPTION|WS_MINIMIZEBOX|WS_CLIPSIBLINGS, CW_USEDEFAULT, CW_USEDEFAULT, Rect.right-Rect.left, Rect.bottom-Rect.top, NULL, NULL, hInstance, NULL);

	ShowWindow(hWnd, SW_SHOW);
	SetForegroundWindow(hWnd);

	if(!CreateOpenGL())
		return -1;

	if(!Init())
	{
		DestroyOpenGL();
		return -1;
	}

	Resize(0, 0, Width, Height);

	if(!CreateDirectSound())
	{
		DestroyOpenGL();
		return -1;
	}

	Frequency=GetFrequency();
	EndTime=rdtsc();

	while(!Done)
	{
		if(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if(msg.message==WM_QUIT)
				Done=1;
			else
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
		else
		{
			if(Active)
			{
				StartTime=EndTime;
				EndTime=rdtsc();

				Render();
				SwapBuffers(hDC);

				fTimeStep=(float)(EndTime-StartTime)/Frequency;
				fTime+=fTimeStep;
				fTimeFPS+=1.0f/fTimeStep;

				if(Frames++>30)
				{
					FPS=fTimeFPS/Frames;
					Frames=0;
					fTimeFPS=0.0f;
				}
			}
			else
				WaitMessage();
		}
	}

	DestroyOpenGL();
	DestroyDirectSound();

	DestroyWindow(hWnd);

	return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
		case WM_CREATE:
			break;

		case WM_CLOSE:
			PostQuitMessage(0);
			break;

		case WM_DESTROY:
			break;

		case WM_SIZE:
			switch(wParam)
			{
				case SIZE_MINIMIZED:
					Active=0;
					break;

				case SIZE_MAXIMIZED:
					Active=1;
					Resize(0, 0, LOWORD(lParam), HIWORD(lParam));
					break;

				case SIZE_RESTORED:
					Active=1;
					Resize(0, 0, LOWORD(lParam), HIWORD(lParam));
					break;
			}
			break;

		case WM_KEYDOWN:
			Key[wParam]=TRUE;

			switch(wParam)
			{
				case VK_ESCAPE:
					PostQuitMessage(0);
					break;

				default:
					break;
			}
			break;

		case WM_KEYUP:
			Key[wParam]=FALSE;
			break;
	}

	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void Render(void)
{
	float x;
	void *Data1=NULL, *Data2=NULL;
	unsigned long Length1=0, Length2=0;
	unsigned int i;

	// Elasped time for sound updates, approx 60/sec
	if(fTime>fSoundUpdate)
	{
		// Lock the sound capture buffer
		if(IDirectSoundCaptureBuffer_Lock(lpDSBCapture, 0, audio_size*sizeof(short)*2, &Data1, &Length1, &Data2, &Length2, DSCBLOCK_ENTIREBUFFER)==DS_OK)
		{
			short *Buffer=(short *)Data1;

			// Fill the left and right channel FFT complex buffer with the audio samples scaled to float
			for(i=0;i<size;i++)
			{
				// In case the FFT buffer is larger than audio buffer
				if(i<audio_size)
					left[i].r=(float)(*Buffer++)/INT16_MAX;
				else
					left[i].r=0.0f;

				left[i].i=0.0f;

				if(i<audio_size)
					right[i].r=(float)(*Buffer++)/INT16_MAX;
				else
					right[i].r=0.0f;

				right[i].i=0.0f;
			}

			// Unlock the sound capture
			IDirectSoundCaptureBuffer_Unlock(lpDSBCapture, Data1,  Length1, Data2, Length2);
		}

		// Run the FFT
		fft(left, left, size, 1);
		fft(right, right, size, 1);

		// Reset timer
		fTime=0.0f;
	}

	// Render a really simple colored peaks with (very) basic OpenGL
	glClear(GL_COLOR_BUFFER_BIT);

	glBindTexture(GL_TEXTURE_1D, 1);
	glEnable(GL_TEXTURE_1D);
	glBegin(GL_TRIANGLE_STRIP);
	for(i=0, x=0.0f;i<=size>>1;i++, x+=delta)
	{
		// Calculate the amplitude from the FFT and clip peak to 0.0-1.0
		float lpeak=min(1.0f, max(0.0f, sqrtf(left[i].r*left[i].r+left[i].i*left[i].i)/(audio_size>>1)));
		// Save old peak value, smaller multipler here will slow down peaks
		float oldlpeak=lpeaks[i]*10.0f;

		if(lpeak>1.0f)
			lpeak=1.0f;

		if(lpeaks[i]<lpeak)
			lpeaks[i]=lpeak;

		lpeaks[i]-=fTimeStep*oldlpeak;

		glColor3f(1.0f, 1.0f, 1.0f);
		glTexCoord1f(0.0f);
		glVertex3f(x, 0.5f, -1.0f);
		glColor3f(0.0f, 0.0f, 1.0f);
		glTexCoord1f(1.0f);
		glVertex3f(x, lpeaks[i]+0.5f, -1.0f);
	}
	glEnd();
	glBegin(GL_TRIANGLE_STRIP);
	for(i=0, x=0.0f;i<=size>>1;i++, x+=delta)
	{
		float rpeak=min(1.0f, max(0.0f, sqrtf(right[i].r*right[i].r+right[i].i*right[i].i)/(audio_size>>1)));
		float oldrpeak=rpeaks[i]*10.0f;

		if(rpeak>1.0f)
			rpeak=1.0f;

		if(rpeaks[i]<rpeak)
			rpeaks[i]=rpeak;

		rpeaks[i]-=fTimeStep*oldrpeak;

		glColor3f(1.0f, 1.0f, 1.0f);
		glTexCoord1f(0.0f);
		glVertex3f(x, 0.5f, -1.0f);
		glColor3f(1.0f, 0.0f, 0.0f);
		glTexCoord1f(1.0f);
		glVertex3f(x, (1.0f-rpeaks[i])-0.5f, -1.0f);
	}
	glEnd();
	glDisable(GL_TEXTURE_1D);

	// Frequency scale and FPS counter, scale is hardcoded to window width!
	BeginOverlay(Width, Height);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
		Font_Print((int)(0.899f*Width), Height/2-8, "|20kHz");
		Font_Print((int)(0.808f*Width), Height/2-8, "|18kHz");
		Font_Print((int)(0.717f*Width), Height/2-8, "|16kHz");
		Font_Print((int)(0.627f*Width), Height/2-8, "|14kHz");
		Font_Print((int)(0.536f*Width), Height/2-8, "|12kHz");
		Font_Print((int)(0.445f*Width), Height/2-8, "|10kHz");
		Font_Print((int)(0.355f*Width), Height/2-8, "|8kHz");
		Font_Print((int)(0.263f*Width), Height/2-8, "|6kHz");
		Font_Print((int)(0.173f*Width), Height/2-8, "|4kHz");
		Font_Print((int)(0.083f*Width), Height/2-8, "|2kHz");
		Font_Print((int)(0.014f*Width), Height/2-8, "|500Hz");
		glBlendFunc(GL_ONE, GL_ONE);
		glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
		Font_Print(0, 0, "FPS: %0.1f", FPS);
	EndOverlay();
}

int Init(void)
{
	const unsigned char Gradient[]=
	{
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0xfe, 0xfe,
		0xfd, 0xfd, 0xfc, 0xfb, 0xfb, 0xfa, 0xf9, 0xf8, 0xf6, 0xf6, 0xf4, 0xf3, 0xf1, 0xf1, 0xef, 0xed,
		0xed, 0xeb, 0xe9, 0xe7, 0xe6, 0xe4, 0xe3, 0xe1, 0xe0, 0xde, 0xdc, 0xdb, 0xd9, 0xd7, 0xd6, 0xd4,
		0xd2, 0xd0, 0xce, 0xcc, 0xcb, 0xc8, 0xc7, 0xc6, 0xc4, 0xc1, 0xc0, 0xbe, 0xbc, 0xba, 0xb7, 0xb7,
		0xb4, 0xb3, 0xb1, 0xae, 0xac, 0xab, 0xa9, 0xa6, 0xa4, 0xa4, 0xa2, 0x9f, 0x9d, 0x9b, 0x9a, 0x97,
		0x95, 0x95, 0x93, 0x91, 0x8f, 0x8c, 0x8c, 0x8a, 0x87, 0x86, 0x85, 0x83, 0x81, 0x7f, 0x7c, 0x7b,
		0x78, 0x76, 0x74, 0x71, 0x6d, 0x6c, 0x68, 0x66, 0x62, 0x60, 0x5d, 0x5a, 0x58, 0x55, 0x52, 0x4f,
		0x4e, 0x4a, 0x47, 0x45, 0x42, 0x3f, 0x3d, 0x3a, 0x37, 0x35, 0x33, 0x30, 0x2e, 0x2b, 0x29, 0x27,
		0x24, 0x23, 0x20, 0x1e, 0x1d, 0x1a, 0x18, 0x15, 0x12, 0x0f, 0x0e, 0x0b, 0x0a, 0x08, 0x06, 0x00
	};

	glBindTexture(GL_TEXTURE_1D, 1);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage1D(GL_TEXTURE_1D, 0, GL_INTENSITY8, 256, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, Gradient);

	glBlendFunc(GL_ONE, GL_ONE);
	glEnable(GL_BLEND);

	memset(left, 0, sizeof(float)*size);
	memset(right, 0, sizeof(float)*size);
	memset(lpeaks, 0, sizeof(float)*size);
	memset(rpeaks, 0, sizeof(float)*size);

	return 1;
}

// DirectSound and OpenGL init code beyond here

int CreateDirectSound(void)
{
    DSCBUFFERDESC dscbd;
	WAVEFORMATEX waveformat;

	if(DirectSoundCaptureCreate(NULL, &lpDSCapture, NULL)!=DS_OK)
	{
		MessageBox(NULL, "DirectSoundCaptureCreate failed.", "Error", MB_OK);
		return 0;
	}

	memset(&waveformat, 0, sizeof(WAVEFORMATEX));
	waveformat.wFormatTag=WAVE_FORMAT_PCM;
	waveformat.nSamplesPerSec=44100;
	waveformat.wBitsPerSample=16;
	waveformat.nChannels=2;
	waveformat.nBlockAlign=waveformat.nChannels*(waveformat.wBitsPerSample>>3);
	waveformat.nAvgBytesPerSec=waveformat.nBlockAlign*waveformat.nSamplesPerSec;
	waveformat.cbSize=0;

	memset(&dscbd, 0, sizeof(DSCBUFFERDESC));
	dscbd.dwSize=sizeof(dscbd);
	dscbd.dwBufferBytes=audio_size*(waveformat.wBitsPerSample>>3)*waveformat.nChannels;
	dscbd.lpwfxFormat=&waveformat;

	if(IDirectSoundCapture_CreateCaptureBuffer(lpDSCapture, &dscbd, &lpDSBCapture, NULL)!=DS_OK)
	{
		MessageBox(NULL, "IDirectSoundCapture_CreateCaptureBuffer failed.", "Error", MB_OK);
		return 0;
	}

	if(lpDSBCapture)
		IDirectSoundCaptureBuffer_Start(lpDSBCapture, DSCBSTART_LOOPING);

	return 1;
}

void DestroyDirectSound(void)
{
	if(lpDSBCapture)
		IDirectSoundCaptureBuffer_Stop(lpDSBCapture);

	if(lpDSBCapture)
	{
		IDirectSoundCaptureBuffer_Release(lpDSBCapture);
		lpDSBCapture=NULL;
	}

	if(lpDSCapture)
	{
		IDirectSoundCapture_Release(lpDSCapture);
		lpDSCapture=NULL;
	}
}

int CreateOpenGL(void)
{
	PIXELFORMATDESCRIPTOR pfd;
	unsigned int PixelFormat;

	memset(&pfd, 0, sizeof(PIXELFORMATDESCRIPTOR));
	pfd.nSize=sizeof(PIXELFORMATDESCRIPTOR);
	pfd.nVersion=1;
	pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
	pfd.iPixelType=PFD_TYPE_RGBA;
	pfd.cColorBits=32;
	pfd.cDepthBits=24;

	hDC=GetDC(hWnd);

	if(!(PixelFormat=ChoosePixelFormat(hDC, &pfd)))
	{
		MessageBox(hWnd, "ChoosePixelFormat failed.", "Error", MB_OK);
		return 0;
	}

	if(!SetPixelFormat(hDC, PixelFormat, &pfd))
	{
		MessageBox(hWnd, "SetPixelFormat failed.", "Error", MB_OK);
		return 0;
	}

	if(!(hRC=wglCreateContext(hDC)))
	{
		MessageBox(hWnd, "wglCreateContext failed.", "Error", MB_OK);
		return 0;
	}

	if(!wglMakeCurrent(hDC, hRC))
	{
		MessageBox(hWnd, "wglMakeCurrent failed.", "Error", MB_OK);
		return 0;
	}

	return 1;
}

void DestroyOpenGL(void)
{
	wglMakeCurrent(NULL, NULL);
	wglDeleteContext(hRC);
	ReleaseDC(hWnd, hDC);
}

void Resize(int x, int y, int Width, int Height)
{
	if(Height==0)
		Height=1;

	glViewport(x, y, Width, Height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}
