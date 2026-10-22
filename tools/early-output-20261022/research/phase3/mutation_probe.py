from pathlib import Path
import subprocess,os,json,hashlib
p=Path(__file__).resolve().parents[2]
s=(p/'research/phase2/probe.c').read_text()
code=r'''
    if (getenv("AU_MUTATION")) {
        const char *mutation=getenv("AU_MUTATION");
        int target=getenv("MUTATION_AU")?atoi(getenv("MUTATION_AU")):12;
        if (target<0 || target>=count) {ret=AVERROR(EINVAL);goto end;}
        AVPacket *packet=packets[target];
        int pos=0,first_slice=-1,second_slice=-1,second_size=0,timing=-1,timing_size=0;
        while(pos+4<=packet->size) {
            unsigned len=AV_RB32(packet->data+pos);
            if(!len || len>packet->size-pos-4) {ret=AVERROR_INVALIDDATA;goto end;}
            int type=packet->data[pos+4]&31;
            if(type==1||type==5) {if(first_slice<0)first_slice=pos;else if(second_slice<0){second_slice=pos;second_size=len;}}
            if(type==6 && len>=3 && packet->data[pos+5]==1) {timing=pos;timing_size=len;}
            pos+=4+len;
        }
        if(first_slice<0 || timing<0) {ret=AVERROR_INVALIDDATA;goto end;}
        uint8_t nal[256]={6,5,16,0,128};
        int size=5,at=first_slice,remove=0;
        if(!strcmp(mutation,"late")) {at=packet->size;size=timing_size;memcpy(nal,packet->data+timing+4,size);nal[3]^=1;}
        else if(!strcmp(mutation,"duplicate")) {size=timing_size;memcpy(nal,packet->data+timing+4,size);}
        else if(!strcmp(mutation,"contradictory")) {size=timing_size;memcpy(nal,packet->data+timing+4,size);nal[3]^=1;}
        else if(!strcmp(mutation,"short")) {at=timing;remove=4+timing_size;size=6;nal[0]=6;nal[1]=1;nal[2]=2;nal[3]=packet->data[timing+7];nal[4]=packet->data[timing+8];nal[5]=128;}
        else if(!strcmp(mutation,"truncated")) {nal[1]=1;nal[2]=16;}
        else if(!strcmp(mutation,"slice-error")) {if(second_slice<0){ret=AVERROR(EINVAL);goto end;}at=second_slice;remove=4+second_size;size=2;nal[0]=0x41;nal[1]=0x80;}
        else if(strcmp(mutation,"parser-error")) {ret=AVERROR(EINVAL);goto end;}
        int oldsize=packet->size,delta=4+size-remove;
        if(delta>=0) {if((ret=av_grow_packet(packet,delta))<0)goto end;}
        else if((ret=av_packet_make_writable(packet))<0)goto end;
        memmove(packet->data+at+4+size,packet->data+at+remove,oldsize-at-remove);
        AV_WB32(packet->data+at,size);memcpy(packet->data+at+4,nal,size);
        if(delta<0)av_shrink_packet(packet,oldsize+delta);
        fprintf(stderr,"mutation=%s target_au=%d inserted_at=%d nal_bytes=%d\n",mutation,target,at,size);
        if(getenv("DUMP_MUTATED_AU")) {FILE *f=fopen(getenv("DUMP_MUTATED_AU"),"wb");if(!f){ret=AVERROR(EIO);goto end;}fwrite(packet->data,1,packet->size,f);fclose(f);}
    }
'''
s=s.replace('    ctx = avcodec_alloc_context3(codec);',code+'\n    ctx = avcodec_alloc_context3(codec);')
(p/'research/phase3/mutation_probe.c').write_text(s)
prefix=p/'out/install'/os.getenv('BUILD','phase2');binary=p/'out/phase3'/('mutation_probe-'+prefix.name)
libs=subprocess.check_output(['pkg-config','--libs','libavformat','libavcodec','libavutil'],text=True,env=dict(os.environ,PKG_CONFIG_PATH=str(prefix/'lib/pkgconfig'))).split()
cmd=['clang','-O2','-g',*(['-fsanitize=address','-fno-omit-frame-pointer'] if prefix.name.endswith('asan') else []),'-I'+str(prefix/'include'),str(p/'research/phase3/mutation_probe.c'),'-o',str(binary),*libs]
subprocess.run(cmd,check=True)
print(binary)
