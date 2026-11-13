// QA tool: records what the default playback device plays (WASAPI loopback) into a 16-bit stereo WAV, for the audio checks (tools/wav_clicks.py).
// Usage: loopback_rec <out.wav> <seconds> [device index]   (no index: the default playback device; index -1 lists the active playback devices)
// Build (llvm-mingw): x86_64-w64-mingw32-clang -O2 -o loopback_rec.exe loopback_rec.c -lole32 -luuid
#define COBJMACROS
#define INITGUID
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

// index -2: the default device; -1: list them; 0...: the n-th active playback device
static HRESULT pick_device(IMMDeviceEnumerator *en, int index, IMMDevice **dev)
{
	if (index == -2)
		return IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, dev);
	IMMDeviceCollection *col = NULL;
	UINT n = 0;
	if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col)) || FAILED(IMMDeviceCollection_GetCount(col, &n)))
		return E_FAIL;
	if (index == -1) {
		for (UINT i = 0; i < n; i++) {
			IMMDevice *d = NULL;
			IPropertyStore *ps = NULL;
			PROPVARIANT v;
			PropVariantInit(&v);
			if (SUCCEEDED(IMMDeviceCollection_Item(col, i, &d)) && SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) && SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)))
				printf("%u %ls\n", i, v.pwszVal);
		}
		exit(0);
	}
	return (UINT)index < n ? IMMDeviceCollection_Item(col, (UINT)index, dev) : E_FAIL;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: loopback_rec out.wav seconds\n");
		return 2;
	}
	const double seconds = atof(argv[2]);
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	IMMDeviceEnumerator *en = NULL;
	IMMDevice *dev = NULL;
	IAudioClient *cl = NULL;
	IAudioCaptureClient *cap = NULL;
	WAVEFORMATEX *wf = NULL;
	if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en)) || FAILED(pick_device(en, argc > 3 ? atoi(argv[3]) : -2, &dev)) ||
	    FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&cl)) || FAILED(IAudioClient_GetMixFormat(cl, &wf)) ||
	    FAILED(IAudioClient_Initialize(cl, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, wf, NULL)) || FAILED(IAudioClient_GetService(cl, &IID_IAudioCaptureClient, (void **)&cap))) {
		fprintf(stderr, "could not open the loopback capture\n");
		return 1;
	}
	const int ch = wf->nChannels, rate = (int)wf->nSamplesPerSec;
	const int is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->wBitsPerSample == 32);
	fprintf(stderr, "format: %d ch, %d Hz, %d bits, %s\n", ch, rate, wf->wBitsPerSample, is_float ? "float" : "int");
	FILE *f = fopen(argv[1], "wb");
	if (!f)
		return 1;
	fwrite("RIFF", 1, 4, f);
	put32(f, 0);
	fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16);
	put16(f, 1);
	put16(f, 2);
	put32(f, (uint32_t)rate);
	put32(f, (uint32_t)rate * 4);
	put16(f, 4);
	put16(f, 16);
	fwrite("data", 1, 4, f);
	put32(f, 0);
	IAudioClient_Start(cl);
	const DWORD t0 = GetTickCount();
	uint32_t frames_total = 0;
	while (GetTickCount() - t0 < (DWORD)(seconds * 1000)) {
		Sleep(5);
		UINT32 n = 0;
		while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(cap, &n)) && n) {
			BYTE *data;
			DWORD flags;
			if (FAILED(IAudioCaptureClient_GetBuffer(cap, &data, &n, &flags, NULL, NULL)))
				break;
			for (UINT32 i = 0; i < n; i++) {
				int16_t out[2] = {0, 0};
				if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
					for (int c = 0; c < 2 && c < ch; c++) {
						float v = is_float ? ((const float *)data)[i * ch + c] : ((const int16_t *)data)[i * ch + c] / 32768.0f;
						if (v > 1)
							v = 1;
						if (v < -1)
							v = -1;
						out[c] = (int16_t)(v * 32767.0f);
					}
					if (ch == 1)
						out[1] = out[0];
				}
				fwrite(out, 2, 2, f);
				frames_total++;
			}
			IAudioCaptureClient_ReleaseBuffer(cap, n);
		}
		{ // the loopback delivers no packets while nothing plays: pad with silence so the file's time is the wall clock's
			const uint32_t target = (uint32_t)((double)(GetTickCount() - t0) * rate / 1000.0);
			static const int16_t zero[2] = {0, 0};
			while (frames_total + rate / 25 < target) {
				fwrite(zero, 2, 2, f);
				frames_total++;
			}
		}
	}
	IAudioClient_Stop(cl);
	fseek(f, 4, SEEK_SET);
	put32(f, 36 + frames_total * 4);
	fseek(f, 40, SEEK_SET);
	put32(f, frames_total * 4);
	fclose(f);
	fprintf(stderr, "%u frames written\n", frames_total);
	return 0;
}
