/*
 * Crumple Zone: host noise probe (runs on the Linux host, not on QNX).
 *
 * Copyright (C) 2026 chinesepowered
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * A 1 ms clock_nanosleep(TIMER_ABSTIME) loop at SCHED_FIFO 30, pinned to one
 * host CPU, measuring wake-up lateness. Whatever this shows is noise the host
 * adds before QEMU and QNX are even involved. usage: hostlat <cpu> [iterations]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sched.h>
#include <stdint.h>
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000000ull+t.tv_nsec;}
static int cmp(const void*a,const void*b){uint64_t x=*(uint64_t*)a,y=*(uint64_t*)b;return x<y?-1:x>y;}
int main(int c,char**v){int cpu=atoi(v[1]);cpu_set_t s;CPU_ZERO(&s);CPU_SET(cpu,&s);sched_setaffinity(0,sizeof s,&s);
struct sched_param p={.sched_priority=30};sched_setscheduler(0,SCHED_FIFO,&p);
int n=c>2?atoi(v[2]):5000;uint64_t*l=malloc(n*8);uint64_t t=ns()+1000000;
for(int i=0;i<n;i++){struct timespec ts={t/1000000000ull,t%1000000000ull};clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&ts,0);l[i]=ns()-t;t+=1000000;}
qsort(l,n,8,cmp);printf("{\"cpu\":%d,\"n\":%d,\"p50_us\":%.1f,\"p99_us\":%.1f,\"p999_us\":%.1f,\"max_us\":%.1f}\n",cpu,n,l[n/2]/1e3,l[n*99/100]/1e3,l[n*999/1000]/1e3,l[n-1]/1e3);}
