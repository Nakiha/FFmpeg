from pathlib import Path
p=Path(__file__).resolve().parents[2];s=(p/'research/phase2/probe.c').read_text()
s=s.replace('#include <poll.h>','#include <poll.h>\n#include <time.h>')
s=s.replace('static atomic_int buffer_gate, gate_entered;', '''static int64_t api_cpu_ns;
static int64_t threadcpu(void) { struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts); return (int64_t)ts.tv_sec*1000000000+ts.tv_nsec; }
static int measured_send(AVCodecContext *ctx,const AVPacket *pkt) { int64_t t=threadcpu(); int r=avcodec_send_packet(ctx,pkt);api_cpu_ns+=threadcpu()-t;return r; }
static int measured_receive(AVCodecContext *ctx,AVFrame *f) { int64_t t=threadcpu(); int r=avcodec_receive_frame(ctx,f);api_cpu_ns+=threadcpu()-t;return r; }
static atomic_int buffer_gate, gate_entered;''')
s=s.replace('legacy ? avcodec_send_packet(ctx, packet)','legacy ? measured_send(ctx, packet)').replace('legacy ? avcodec_send_packet(ctx, NULL)','legacy ? measured_send(ctx, NULL)').replace('legacy ? avcodec_receive_frame(ctx, frame)','legacy ? measured_receive(ctx, frame)')
s=s.replace('    for (cycle = 0; cycle < cycles; cycle++) {', '    int64_t decode_began=av_gettime_relative();\n    for (cycle = 0; cycle < cycles; cycle++) {')
s=s.replace('    struct rusage usage; getrusage', '    fprintf(stderr,"decode_wall_us=%lld api_caller_cpu_ns=%lld\\n",(long long)(av_gettime_relative()-decode_began),(long long)api_cpu_ns);\n    struct rusage usage; getrusage')
s=s.replace('now - stamp->submitted_us);','now - stamp->submitted_us);')
s=s.replace('"frame,%d,%d,%" PRId64 ",%" PRId64 "\\n",','"frame,%d,%d,%" PRId64 ",%" PRId64 ",%d\\n",').replace('cycle, frames, stamp->sequence, now - stamp->submitted_us);','cycle, frames, stamp->sequence, now - stamp->submitted_us,next-frames-1);')
s=s.replace('now - stamp->submitted_us,next-frames-1);','now - stamp->submitted_us,next-frames-1, fps ? now-(began+stamp->sequence*1000000/fps) : 0);')
s=s.replace('PRId64 ",%d\\n",','PRId64 ",%d,%lld\\n",')
s=s.replace('fps ? now-(began+stamp->sequence*1000000/fps) : 0);','(long long)(fps ? now-(began+stamp->sequence*1000000/fps) : 0));')
(p/'research/phase2/perf_probe.c').write_text(s)
