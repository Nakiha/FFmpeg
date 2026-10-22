from pathlib import Path
p=Path(__file__).resolve().parents[1]
s=(p/'FFmpeg/tools/live-async/async_probe.c').read_text()
s=s.replace('#include <sys/eventfd.h>','#include <fcntl.h>\n#include <errno.h>\n#include <sys/resource.h>')
s=s.replace('    int fd;','    int fd, write_fd;')
s=s.replace('write(event->fd, &one, sizeof(one))','write(event->write_fd, &one, sizeof(one))')
s=s.replace('event.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);','int pipefd[2];\n    if (pipe(pipefd)) return 2;\n    event.fd=pipefd[0]; event.write_fd=pipefd[1];\n    fcntl(event.fd,F_SETFL,O_NONBLOCK); fcntl(event.write_fd,F_SETFL,O_NONBLOCK);')
s=s.replace('close(event.fd);','close(event.fd); close(event.write_fd);')
s=s.replace('if (threads < 2 || fps < 0','if (threads < 1 || fps < 0')
s=s.replace('            packets[count] = av_packet_clone(read_packet);','''            if (getenv("SYNTHETIC_PTS")) {
                read_packet->pts = read_packet->dts = av_rescale_q(count,(AVRational){1,25},format->streams[stream]->time_base);
                read_packet->duration = av_rescale_q(1,(AVRational){1,25},format->streams[stream]->time_base);
            }
            packets[count] = av_packet_clone(read_packet);''')
s=s.replace('    if (!strcmp(argv[4], "checksum"))\n', '''    if (codec->id == AV_CODEC_ID_H264 && getenv("H264_EARLY") &&
        (ret=av_opt_set_int(ctx->priv_data,"live_idr_early_output",atoi(getenv("H264_EARLY")),0))<0) goto end;
    if (getenv("STRICT_ERRORS")) ctx->err_recognition |= AV_EF_EXPLODE;
    if (!strcmp(argv[4], "checksum"))
''',1)
s=s.replace('    while ((ret = av_read_frame(format, read_packet)) >= 0) {', '''    if (getenv("SEEK_US")) {
        int64_t target=av_rescale_q(atoll(getenv("SEEK_US")),AV_TIME_BASE_Q,time_base);
        if ((ret=av_seek_frame(format,stream,target,AVSEEK_FLAG_BACKWARD))<0) goto end;
    }
    while ((ret = av_read_frame(format, read_packet)) >= 0) {''')
# Output the source input ID, field flags and all frame error bits as well as the original metadata.
s=s.replace('        fputc(\'\\n\', output);', '''        const Stamp *st=frame->opaque_ref ? (const Stamp *)frame->opaque_ref->data : NULL;
        fprintf(output,",%d,%d,%d,%d,%" PRId64 "\\n",frame->flags,frame->repeat_pict,frame->decode_error_flags,frame->pict_type,st?st->sequence:-1);''')
s=s.replace('    ret = 0;\nend:', '''    ret = 0;
end:
    fprintf(stderr,"terminal_return=%d\\n",ret);''')
s=s.replace('            if ((ret = avcodec_async_flush(async)) < 0) goto end;\n            began = av_gettime_relative();','            fprintf(stderr,"injected_error=%d\\n",ret);\n            if ((ret = avcodec_async_flush(async)) < 0) goto end;\n            began = av_gettime_relative();')
# Record callback output ordering and packet trigger for debugging. Optional no-hash mode for perf.
s=s.replace('if ((ret = digest_frame(digest, frame, time_base)) < 0) goto end;', 'if (!getenv("NO_HASH") && (ret = digest_frame(digest, frame, time_base)) < 0) goto end;')
s=s.replace('    if (async) avcodec_async_get_stats(async, &stats);\n    fprintf(csv,', '''    struct rusage usage; getrusage(RUSAGE_SELF,&usage);
    fprintf(stderr,"cpu_us=%lld\\n",(long long)(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*1000000+usage.ru_utime.tv_usec+usage.ru_stime.tv_usec);
    if (async) avcodec_async_get_stats(async, &stats);
    fprintf(csv,''')
s=s.replace('        if (frames != count) { ret = AVERROR_INVALIDDATA; goto end; }','')
s=s.replace('total_latency / total_frames','total_frames ? total_latency / total_frames : 0')
(p/'research/portable_probe.c').write_text(s)
