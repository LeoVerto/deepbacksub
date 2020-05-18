/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// tested against tensorflow lite v2.1.0 (static library)

#include <unistd.h>
#include <cstdio>
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"
#include "tensorflow/lite/optional_debug_tools.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/types_c.h>

#include "loopback.h"
#include "capture.h"

// Tensorflow Lite helper functions
using namespace tflite;

#define TFLITE_MINIMAL_CHECK(x)                              \
  if (!(x)) {                                                \
	fprintf(stderr, "Error at %s:%d\n", __FILE__, __LINE__); \
	exit(1);                                                 \
  }

std::unique_ptr<Interpreter> interpreter;

cv::Mat getTensorMat(int tnum, int debug) {

	TfLiteType t_type = interpreter->tensor(tnum)->type;
	TFLITE_MINIMAL_CHECK(t_type == kTfLiteFloat32);

	TfLiteIntArray* dims = interpreter->tensor(tnum)->dims;
	if (debug) for (int i = 0; i < dims->size; i++) printf("tensor #%d: %d\n",tnum,dims->data[i]);
	TFLITE_MINIMAL_CHECK(dims->data[0] == 1);
	
	int h = dims->data[1];
	int w = dims->data[2];
	int c = dims->data[3];

	float* p_data = interpreter->typed_tensor<float>(tnum);
	TFLITE_MINIMAL_CHECK(p_data != nullptr);

	return cv::Mat(h,w,CV_32FC(c),p_data);
}

// deeplabv3 classes
std::vector<std::string> labels = { "background", "aeroplane", "bicycle", "bird", "boat", "bottle", "bus", "car", "cat", "chair", "cow", "dining table", "dog", "horse", "motorbike", "person", "potted plant", "sheep", "sofa", "train", "tv" };

int main(int argc, char* argv[]) {

	printf("deepseg v0.1.0\n");
	printf("(c) 2020 by floe@butterbrot.org\n");
	printf("https://github.com/floe/deepseg\n");

	int debug  = 0;
	int threads= 2;
	int width  = 640;
	int height = 480;
	const char *back = "background.png";
	const char *vcam = "/dev/video0";
	const char *ccam = "/dev/video1";

	const char* modelname = "deeplabv3_257_mv_gpu.tflite";

	for (int arg=1; arg<argc; arg++) {
		if (strncmp(argv[arg], "-?", 2)==0) {
			fprintf(stderr, "usage: deepseg [-?] [-d] [-c <capture:/dev/video1>] [-v <vcam:/dev/video0>] [-w <width:640>] [-h <height:480>] [-t <threads:2>] [-b <background.png>]\n");
			exit(0);
		} else if (strncmp(argv[arg], "-d", 2)==0) {
			++debug;
		} else if (strncmp(argv[arg], "-v", 2)==0) {
			vcam = argv[++arg];
		} else if (strncmp(argv[arg], "-c", 2)==0) {
			ccam = argv[++arg];
		} else if (strncmp(argv[arg], "-b", 2)==0) {
			back = argv[++arg];
		} else if (strncmp(argv[arg], "-w", 2)==0) {
			sscanf(argv[++arg], "%d", &width);
		} else if (strncmp(argv[arg], "-h", 2)==0) {
			sscanf(argv[++arg], "%d", &height);
		} else if (strncmp(argv[arg], "-t", 2)==0) {
			sscanf(argv[++arg], "%d", &threads);
		}
	}
	printf("debug:  %d\n", debug);
	printf("ccam:   %s\n", ccam);
	printf("vcam:   %s\n", vcam);
	printf("width:  %d\n", width);
	printf("height: %d\n", height);
	printf("back:   %s\n", back);
	printf("threads:%d\n", threads);

	// open loopback virtual camera stream, always with YUV420p output
	int lbfd = loopback_init(vcam,width,height,debug);
	// open capture device stream, pass in/out expected/actual size
	int capw = width, caph = height;
	capinfo_t *pcap = capture_init(ccam, &capw, &caph, debug);
	TFLITE_MINIMAL_CHECK(pcap!=NULL);
	printf("stream info:\n");
	printf("  width:  %d\n", capw);
	printf("  height: %d\n", caph);
	printf("  rate:   %d\n", pcap->rate);
	// check background file extension (yeah, I know) to spot videos..
	cv::Mat bg;
	capinfo_t *pbkg = NULL;
	int bkgw = width, bkgh = height;
	char *dot = rindex((char*)back, '.');
	if (dot!=NULL &&
		(strcasecmp(dot, ".png")==0 ||
		 strcasecmp(dot, ".jpg")==0 ||
		 strcasecmp(dot, ".jpeg")==0)) {
		// read background into raw BGR24 format, resize to output
		bg = cv::imread(back);
		cv::resize(bg,bg,cv::Size(width,height));
	} else {
		// assume video background..
		pbkg = capture_init(back, &bkgw, &bkgh, debug);
		TFLITE_MINIMAL_CHECK(pbkg!=NULL);
	}

	// Load model
	std::unique_ptr<tflite::FlatBufferModel> model =
		tflite::FlatBufferModel::BuildFromFile(modelname);
	TFLITE_MINIMAL_CHECK(model != nullptr);

	// Build the interpreter
	tflite::ops::builtin::BuiltinOpResolver resolver;
	InterpreterBuilder builder(*model, resolver);
	builder(&interpreter);
	TFLITE_MINIMAL_CHECK(interpreter != nullptr);

	// Allocate tensor buffers.
	TFLITE_MINIMAL_CHECK(interpreter->AllocateTensors() == kTfLiteOk);

	// set interpreter params
	interpreter->SetNumThreads(threads);
	interpreter->SetAllowFp16PrecisionForFp32(true);

	// get input and output tensor as cv::Mat
	cv::Mat  input = getTensorMat(interpreter->inputs ()[0],debug);
 	cv::Mat output = getTensorMat(interpreter->outputs()[0],debug);
	TFLITE_MINIMAL_CHECK( input.rows ==  input.cols);
	TFLITE_MINIMAL_CHECK(output.rows == output.cols);

	// initialize mask and square ROI in center
	cv::Rect roidim = cv::Rect((width-height)/2,0,height,height);
	cv::Mat mask = cv::Mat::zeros(height,width,CV_32FC1);
	cv::Mat mroi = mask(roidim);

	// erosion/dilation elements
	cv::Mat element3 = cv::getStructuringElement( cv::MORPH_ELLIPSE, cv::Size(3,3) );
	cv::Mat element7 = cv::getStructuringElement( cv::MORPH_ELLIPSE, cv::Size(7,7) );
	cv::Mat element11 = cv::getStructuringElement( cv::MORPH_ELLIPSE, cv::Size(11,11) );

	const int cnum = labels.size();
	const int pers = std::find(labels.begin(),labels.end(),"person") - labels.begin();

	// stats
	int64 es = cv::getTickCount();
	int64 e1 = es;
	int64 fr = 0;
	while (true) {

		// grab next available background frame (if video)
		if (pbkg!=NULL) {
			bg = *(capture_frame(pbkg));
			// resize to output if required
			if (bkgw != width || bkgh != height)
				cv::resize(bg,bg,cv::Size(width,height));
		}

		// grab next available video frame
		cv::Mat raw = *(capture_frame(pcap));
		// resize to output if required
		if (capw != width || caph != height)
			cv::resize(raw,raw,cv::Size(width,height));

		// map ROI
		cv::Mat roi = raw(roidim);
		// convert BGR to RGB, resize ROI to input size
		cv::Mat in_u8_rgb, in_resized;
		cv::cvtColor(roi,in_u8_rgb,CV_BGR2RGB);
		// TODO: can convert directly to float?
		cv::resize(in_u8_rgb,in_resized,cv::Size(input.cols,input.rows));

		// convert to float and normalize values to [-1;1]
		in_resized.convertTo(input,CV_32FC3,1.0/128.0,-1.0);

		// Run inference
		TFLITE_MINIMAL_CHECK(interpreter->Invoke() == kTfLiteOk);

		// create Mat for small mask
		cv::Mat ofinal(output.rows,output.cols,CV_32FC1);
		float* tmp = (float*)output.data;
		float* out = (float*)ofinal.data;

		// find class with maximum probability
		for (unsigned int n = 0; n < output.total(); n++) {
			float maxval = -10000; int maxpos = 0;
			for (int i = 0; i < cnum; i++) {
				if (tmp[n*cnum+i] > maxval) {
					maxval = tmp[n*cnum+i];
					maxpos = i;
				}
			}
			// set mask to 1.0 where class == person
			out[n] = (maxpos==pers ? 1.0 : 0);
		}

		// denoise, close & open with small then large elements, adapted from:
		// https://stackoverflow.com/questions/42065405/remove-noise-from-threshold-image-opencv-python
		if (getenv("DEEPSEG_NODENOISE")==NULL) {
			cv::morphologyEx(ofinal,ofinal,CV_MOP_CLOSE,element3);
			cv::morphologyEx(ofinal,ofinal,CV_MOP_OPEN,element3);
			cv::morphologyEx(ofinal,ofinal,CV_MOP_CLOSE,element7);
			cv::morphologyEx(ofinal,ofinal,CV_MOP_OPEN,element7);
			cv::dilate(ofinal,ofinal,element11);
		}
		// smooth mask edges
		if (getenv("DEEPSEG_NOBLUR")==NULL)
			cv::blur(ofinal,ofinal,cv::Size(7,7));
		// scale up into full-sized mask
		cv::resize(ofinal,mroi,cv::Size(mroi.cols,mroi.rows));
		// alpha blend raw and background images using mask, adapted from:
		// https://www.learnopencv.com/alpha-blending-using-opencv-cpp-python/
		uint8_t *rptr = (uint8_t*)raw.data;
		uint8_t *bptr = (uint8_t*)bg.data;
		float   *aptr = (float*)mask.data;
		int npix = raw.rows * raw.cols;
		for (int pix=0; pix<npix; ++pix) {
			// blending weights
			float rw=*aptr, bw=1.0-rw;
			// blend each channel byte
			*rptr = (uint8_t)( (float)(*rptr)*rw + (float)(*bptr)*bw ); ++rptr; ++bptr;
			*rptr = (uint8_t)( (float)(*rptr)*rw + (float)(*bptr)*bw ); ++rptr; ++bptr;
			*rptr = (uint8_t)( (float)(*rptr)*rw + (float)(*bptr)*bw ); ++rptr; ++bptr;
			++aptr;
		}

		// write frame to v4l2loopback
		cv::Mat yuv;
		cv::cvtColor(raw,yuv,CV_BGR2YUV_I420);
		int framesize = yuv.step[0]*yuv.rows;
		int ret = write(lbfd,yuv.data,framesize);
		TFLITE_MINIMAL_CHECK(ret == framesize);
		++fr;

		if (!debug) { printf("."); fflush(stdout); continue; }

		int64 e2 = cv::getTickCount();
		float el = (e2-e1)/cv::getTickFrequency();
		float t = (e2-es)/cv::getTickFrequency();
		e1 = e2;
		printf("\relapsed:%0.3f gr=%ld gps:%3.1f br=%ld fr=%ld fps:%3.1f   ",
			el, pcap->cnt, pcap->cnt/t, pbkg!=NULL ? pbkg->cnt : 0, fr, fr/t);
		fflush(stdout);
		if (debug > 1) {
			cv::imshow("Deepseg:output",raw);
			if (cv::waitKey(1) == 'q') break;
		}
	}
	capture_stop(pcap);
	if (pbkg!=NULL)
		capture_stop(pbkg);

	return 0;
}

