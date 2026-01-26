typedef struct {
	long wav_offset;
	int duration;
	int padding;
} ASAP_Workload;

kernel void asap2wav(constant char *filename, global const uchar *module, int module_len, global uchar *wav, constant ASAP_Workload *workloads)
{
	int song = get_global_id(0);
	constant ASAP_Workload *workload = workloads + song;
	ASAP asap;
	ASAP_Construct(&asap);
	if (!ASAP_Load(&asap, filename, module, module_len)
	 || !ASAP_PlaySong(&asap, song, workload->duration)) {
		wav[0] = '\0';
		return;
	}
	int wav_len = workload[1].wav_offset - workload->wav_offset;
	wav += workload->wav_offset;
	int header_len = ASAP_GetWavHeader(&asap, wav, ASAPSampleFormat_S16_L_E, false);
	ASAP_Generate(&asap, wav + header_len, wav_len - header_len, ASAPSampleFormat_S16_L_E);
}
