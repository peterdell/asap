/*
 * asapcl.cpp - converter of ASAP-supported formats to WAV files
 *
 * Copyright (C) 2021-2026  Piotr Fusik
 *
 * This file is part of ASAP (Another Slight Atari Player),
 * see http://asap.sourceforge.net
 *
 * ASAP is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * ASAP is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ASAP; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>

#include "asap.h"

void check_error(int err)
{
	if (err != CL_SUCCESS) {
		fprintf(stderr, "OpenCL error %d\n", err);
		exit(1);
	}
}

typedef struct {
	cl_long wav_offset;
	cl_int duration;
	cl_int padding;
} ASAP_Workload;

int main(int argc, char **argv)
{
	cl_platform_id platform;
	cl_uint num;
	check_error(clGetPlatformIDs(1, &platform, &num));
	if (num == 0) {
		fprintf(stderr, "No OpenCL platforms\n");
		return 1;
	}

	cl_device_id device;
	check_error(clGetDeviceIDs(platform, CL_DEVICE_TYPE_DEFAULT, 1, &device, &num));
	if (num == 0) {
		fprintf(stderr, "No OpenCL device\n");
		return 1;
	}

	size_t size;
	check_error(clGetDeviceInfo(device, CL_DEVICE_NAME, 0, nullptr, &size));
	std::unique_ptr<char[]> name(new char[size]);
	check_error(clGetDeviceInfo(device, CL_DEVICE_NAME, size, name.get(), nullptr));
	fprintf(stderr, "Running on %s\n", name.get());

	const cl_context_properties properties[] = {
		CL_CONTEXT_PLATFORM, (cl_context_properties) platform, 0
	};
	cl_int err;
	cl_context context = clCreateContext(properties, 1, &device, nullptr, nullptr, &err);
	check_error(err);

	const char *source =
#include "asap-cl.h"
		;
	cl_program program = clCreateProgramWithSource(context, 1, &source, nullptr, &err);
	check_error(err);

	err = clBuildProgram(program, 1, &device, "-cl-std=CL2.0", nullptr, nullptr);
	if (err == CL_BUILD_PROGRAM_FAILURE) {
		check_error(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size));
		std::unique_ptr<char[]> log(new char[size]);
		check_error(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, size, log.get(), nullptr));
		fprintf(stderr, "OpenCL build error:\n%s", log.get());
		return 1;
	}
	check_error(err);

	cl_kernel kernel = clCreateKernel(program, "asap2wav", &err);
	check_error(err);

	cl_command_queue queue = clCreateCommandQueueWithProperties(context, device, nullptr, &err);
	check_error(err);

	ASAPInfo *info = ASAPInfo_New();
	int exit_code = 0;

	for (int argi = 1; argi < argc; argi++) {
		const char *input_file = argv[argi];
		const char *input_dot = strrchr(input_file, '.');
		if (input_dot == nullptr) {
			fprintf(stderr, "%s: missing filename extension\n", input_file);
			exit_code = 1;
			continue;
		}

		FILE *fp = fopen(input_file, "rb");
		if (fp == NULL) {
			fprintf(stderr, "Cannot open %s\n", input_file);
			exit_code = 1;
			continue;
		}
		static uint8_t module[ASAPInfo_MAX_MODULE_LENGTH];
		int module_len = fread(module, 1, sizeof(module), fp);
		fclose(fp);

		if (!ASAPInfo_Load(info, input_file, module, module_len)) {
			fprintf(stderr, "%s: unsupported file\n", input_file);
			exit_code = 1;
			continue;
		}
		int channels = ASAPInfo_GetChannels(info);
		int songs = ASAPInfo_GetSongs(info);

		fprintf(stderr, "Converting %s\n", input_file);
		size_t wav_len = 0;
		std::unique_ptr<ASAP_Workload[]> workloads(new ASAP_Workload[songs + 1]);
		for (int song = 0; song < songs; song++) {
			int duration = ASAPInfo_GetDuration(info, song);
			if (duration < 0)
				duration = 180 * 1000;
			int song_wav_len = 44 + duration * (ASAP_SAMPLE_RATE / 100) / 10 * channels * 2;
			workloads[song].wav_offset = wav_len;
			workloads[song].duration = duration;
			wav_len += song_wav_len;
		}
		workloads[songs].wav_offset = wav_len;

		cl_mem filename_buffer = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR | CL_MEM_HOST_NO_ACCESS, strlen(input_file) + 1, const_cast<char *>(input_file), &err);
		check_error(err);
		check_error(clSetKernelArg(kernel, 0, sizeof(filename_buffer), &filename_buffer));

		cl_mem module_buffer = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR | CL_MEM_HOST_NO_ACCESS, module_len, module, &err);
		check_error(err);
		check_error(clSetKernelArg(kernel, 1, sizeof(module_buffer), &module_buffer));
		check_error(clSetKernelArg(kernel, 2, sizeof(module_len), &module_len));

		cl_mem wav_buffer = clCreateBuffer(context, CL_MEM_WRITE_ONLY | CL_MEM_HOST_READ_ONLY, wav_len, nullptr, &err);
		check_error(err);
		check_error(clSetKernelArg(kernel, 3, sizeof(wav_buffer), &wav_buffer));

		cl_mem workloads_buffer = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR | CL_MEM_HOST_NO_ACCESS, (songs + 1) * sizeof(ASAP_Workload), workloads.get(), &err);
		check_error(err);
		check_error(clSetKernelArg(kernel, 4, sizeof(workloads_buffer), &workloads_buffer));

		const size_t dim = songs;
		check_error(clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &dim, nullptr, 0, nullptr, nullptr));

		std::unique_ptr<uint8_t[]> wav(new uint8_t[wav_len]);
		check_error(clEnqueueReadBuffer(queue, wav_buffer, false, 0, wav_len, wav.get(), 0, nullptr, nullptr));

		check_error(clFinish(queue));

		for (int song = 0; song < songs; song++) {
			const uint8_t *song_wav = wav.get() + workloads[song].wav_offset;
			if (song_wav[0] != 'R') {
				fprintf(stderr, "%s: conversion error\n", input_file);
				exit_code = 1;
				break;
			}
	
			char output_file[FILENAME_MAX];
			int output_file_len = songs == 1 ? snprintf(output_file, sizeof(output_file), "%.*s.wav", (int) (input_dot - input_file), input_file)
			                                 : snprintf(output_file, sizeof(output_file), "%.*s (song %d).wav", (int) (input_dot - input_file), input_file, song + 1);
			if (output_file_len >= static_cast<int>(sizeof(output_file))) {
				fprintf(stderr, "%s: filename too long\n", input_file);
				exit_code = 1;
				break;
			}

			fp = fopen(output_file, "wb");
			if (fp == NULL) {
				fprintf(stderr, "Cannot create %s\n", output_file);
				exit_code = 1;
				break;
			}
			int song_wav_len = workloads[song + 1].wav_offset - workloads[song].wav_offset;
			fwrite(song_wav, 1, song_wav_len, fp);
			fclose(fp);
		}

		check_error(clReleaseMemObject(workloads_buffer));
		check_error(clReleaseMemObject(wav_buffer));
		check_error(clReleaseMemObject(module_buffer));
		check_error(clReleaseMemObject(filename_buffer));
	}

	ASAPInfo_Delete(info);

	check_error(clReleaseCommandQueue(queue));
	check_error(clReleaseKernel(kernel));
	check_error(clReleaseProgram(program));
	check_error(clReleaseContext(context));
	return exit_code;
}
