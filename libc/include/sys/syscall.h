/* Copyright 2016 Pedro Falcato

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http ://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef SYSCALL_H
#define SYSCALL_H

#define TERMINAL_WRITE_SYSCALL 0L
#define FORK_SYSCALL 	1L
#define EXIT_SYSCALL 	2L
#define MMAP_SYSCALL 	3L
#define EXEC_SYSCALL 	4L
#define ABORT_SYSCALL 	5L

#define SYSCALL(intno,ebxr,ecxr,edxr,edir,esir) \
asm volatile("movl %0,%%eax"::"a"(intno)); \
asm volatile("movl %0,%%ebx"::"a"(ebxr)); \
asm volatile("movl %0,%%ecx"::"a"(ecxr)); \
asm volatile("movl %0,%%edx"::"a"(edxr)); \
asm volatile("movl %0,%%edi"::"a"(edir)); \
asm volatile("movl %0,%%esi"::"a"(esir)); \
asm volatile("int $0x80"); 



#endif // SYSCALL_H
