#define _POSIX_C_SOURCE 200809L

#include "frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Repl {
	CompileContext ctx;
	STACK(Frontend*) frontends;
	STACK(char*) names;
	STACK(char) source;
	VM vm;
	bool defining;
	char* pending_name;
} Repl;

static SigInput print_inputs[] = {{.var = {.tid = TYPE_INT_ID,.name = "value"}}};

static Type repl_types[] = {
	[0] = {.kind = TYPE_INT,.name = "int",.payload_size = sizeof(num_t),.size = sizeof(num_t),.align = alignof(num_t)},
	[1] = {.kind = TYPE_BYTE,.name = "byte",.payload_size = 1,.size = 1,.align = 1},
	[2] = {
		.kind = TYPE_NATIVE_FUNC_POINTER,
		.name = "print_int",
		.payload_size = sizeof(VmNativeFunc),
		.size = sizeof(VmNativeFunc),
		.align = alignof(VmNativeFunc),
		.data.sig.ins = {.data = print_inputs,.len = 1},
	},
};

static VM_RESULT print_int(VM* vm){
	if(!vm->param_stack.len) return VM_PARAM_UNDERFLOW;
	printf("%lld\n",(long long)*(const num_t*)TOP(vm->param_stack));
	return VM_OK;
}

static VmNativeFunc print_int_fn = print_int;

static bool grow(void** data,size_t* cap,size_t need,size_t elem_size){
	if(need <= *cap) return true;
	size_t next = *cap ? *cap * 2 : 8;
	while(next < need) next *= 2;
	void* resized = realloc(*data,next * elem_size);
	if(!resized) return false;
	*data = resized;
	*cap = next;
	return true;
}

static char* trim(char* line){
	size_t whitespace;
	while((whitespace = frontend_whitespace_len(line))) line += whitespace;
	char* p = line;
	char* end = line;
	while(*p){
		if((whitespace = frontend_whitespace_len(p))) p += whitespace;
		else end = ++p;
	}
	*end = 0;
	return line;
}

static bool append_source(Repl* repl,const char* line){
	size_t len = strlen(line);
	if(!grow((void**)&repl->source.data,&repl->source.cap,repl->source.len + len + 2,sizeof(char))) return false;
	memcpy(repl->source.data + repl->source.len,line,len);
	repl->source.len += len;
	repl->source.data[repl->source.len++] = '\n';
	repl->source.data[repl->source.len] = 0;
	return true;
}

static const char* frontend_error_name(FrontendError error){
	switch(error){
	case FRONTEND_OK: return "ok";
	case FRONTEND_OOM: return "out of memory";
	case FRONTEND_UNKNOWN_WORD: return "unknown word";
	case FRONTEND_BAD_TOKEN: return "bad token";
	case FRONTEND_EXPECTED_NAME: return "expected name";
	case FRONTEND_BAD_NUMBER: return "bad number";
	case FRONTEND_UNKNOWN_TYPE: return "unknown type";
	case FRONTEND_DUPLICATE_VAR: return "duplicate variable";
	case FRONTEND_MACRO_COMPILE_FAILED: return "immediate compilation failed";
	case FRONTEND_MACRO_RUNTIME_FAILED: return "immediate execution failed";
	case FRONTEND_INTERPRETER_COMPILE_FAILED: return "interpreter compilation failed";
	case FRONTEND_INTERPRETER_RUNTIME_FAILED: return "interpreter execution failed";
	case FRONTEND_BAD_CONTROL_FLOW: return "invalid or incomplete control flow";
	}
	return "frontend error";
}

static void report_frontend_error(const Frontend* fe){
	fprintf(stderr,"error: %s",frontend_error_name(fe->error));
	if(fe->error_word){
		size_t len = 0;
		while(fe->error_word[len] && !frontend_whitespace_len(fe->error_word + len) &&
			fe->error_word[len] != '(' && fe->error_word[len] != ')') len++;
		fprintf(stderr," near '%.*s'",(int)len,fe->error_word);
	}
	fputc('\n',stderr);
}

static bool add_function_words(Frontend* fe,const Repl* repl,size_t len){
	for(func_idx i=0;i<len;i++){
		if(!frontend_add_word_func(fe,repl->ctx.funcs.data[i].name,i)) return false;
	}
	return true;
}

static void free_built_func(Frontend* fe,Func* func){
	free(func->blocks.data);
	free(func->ops.data);
	if(fe->var_cap) free(func->vars.data);
	if(fe->type_cap) free(func->types.data);
	frontend_free(fe);
}

static void lower_func_defers(Func* func){
	for(size_t i=0;i<func->blocks.len;i++){
		if(func->blocks.data[i].kind == BLOCK_DEFER){
			remove_defers(&func->blocks);
			return;
		}
	}
}

static bool build_frontend(Repl* repl,Frontend* fe,Func* func,size_t word_count){
	frontend_init(fe,&repl->ctx,func);
	return frontend_add_core_words(fe)
		&& frontend_add_word_native(fe,"Print",0)
		&& add_function_words(fe,repl,word_count);
}

static bool run_anonymous(Repl* repl){
	Func func = {.name = "<repl>",.types = {.data = repl_types,.len = 3}};
	Frontend fe;
	if(!build_frontend(repl,&fe,&func,repl->ctx.funcs.len) ||
		!frontend_compile_source(&fe,repl->source.data ? repl->source.data : "")){
		report_frontend_error(&fe);
		free_built_func(&fe,&func);
		return false;
	}

	lower_func_defers(&func);
	VmCode code = vm_compile_no_defers(&func,&repl->ctx);
	if(!code.data){
		fprintf(stderr,"error: generated IR did not compile\n");
		free_built_func(&fe,&func);
		return false;
	}
	VM_RESULT result = vm_run(&repl->vm,code.data);
	vm_code_free(&code);
	free_built_func(&fe,&func);
	if(result != VM_OK){
		fprintf(stderr,"error: VM returned %d\n",result);
		return false;
	}
	puts("ran");
	return true;
}

static bool name_exists(const Repl* repl,const char* name){
	for(size_t i=0;i<repl->ctx.funcs.len;i++){
		if(strcmp(repl->ctx.funcs.data[i].name,name) == 0) return true;
	}
	return false;
}

static bool define_named(Repl* repl){
	if(name_exists(repl,repl->pending_name)){
		fprintf(stderr,"error: function '%s' is already defined\n",repl->pending_name);
		return false;
	}
	if(repl->ctx.funcs.len > (func_idx)-1 ||
		!grow((void**)&repl->ctx.funcs.data,&repl->ctx.funcs.cap,repl->ctx.funcs.len + 1,sizeof(Func)) ||
		!grow((void**)&repl->frontends.data,&repl->frontends.cap,repl->frontends.len + 1,sizeof(Frontend*)) ||
		!grow((void**)&repl->names.data,&repl->names.cap,repl->names.len + 1,sizeof(char*))) return false;

	Frontend* fe = malloc(sizeof(*fe));
	if(!fe) return false;
	func_idx idx = (func_idx)repl->ctx.funcs.len;
	Func* func = &repl->ctx.funcs.data[repl->ctx.funcs.len++];
	*func = (Func){.name = repl->pending_name,.types = {.data = repl_types,.len = 3}};
	if(!build_frontend(repl,fe,func,idx) ||
		!frontend_compile_source(fe,repl->source.data ? repl->source.data : "")){
		report_frontend_error(fe);
		free_built_func(fe,func);
		free(fe);
		repl->ctx.funcs.len--;
		return false;
	}

	lower_func_defers(func);
	VmCode code = vm_compile_no_defers(func,&repl->ctx);
	if(!code.data){
		fprintf(stderr,"error: generated IR for '%s' did not compile\n",repl->pending_name);
		free_built_func(fe,func);
		free(fe);
		repl->ctx.funcs.len--;
		return false;
	}
	vm_code_free(&code);
	repl->frontends.data[repl->frontends.len++] = fe;
	repl->names.data[repl->names.len++] = repl->pending_name;
	printf("defined %s [%u]\n",repl->pending_name,(unsigned)idx);
	repl->pending_name = NULL;
	return true;
}

static void finish_form(Repl* repl){
	if(repl->defining) define_named(repl);
	else run_anonymous(repl);
	if(repl->pending_name){
		free(repl->pending_name);
		repl->pending_name = NULL;
	}
	repl->source.len = 0;
	if(repl->source.data) repl->source.data[0] = 0;
	repl->defining = false;
}

static bool begin_definition(Repl* repl,const char* line){
	if(strncmp(line,"Func",4) != 0 || (line[4] && !frontend_whitespace_len(line + 4))) return false;
	char* copy = strdup(line + 4);
	if(!copy){
		fprintf(stderr,"error: out of memory\n");
		return true;
	}
	char* name = trim(copy);
	char* body = name;
	while(*body && !frontend_whitespace_len(body) && *body != '(' && *body != ')') body++;
	size_t separator_len = frontend_whitespace_len(body);
	if(separator_len){
		*body = 0;
		body += separator_len;
	}else if(*body){
		*body++ = 0;
	}
	if(!name[0]){
		fprintf(stderr,"error: Func expects a name\n");
		free(copy);
		return true;
	}
	repl->pending_name = strdup(name);
	if(!repl->pending_name){
		fprintf(stderr,"error: out of memory\n");
		free(copy);
		return true;
	}
	repl->defining = true;
	if(body[0] && !append_source(repl,trim(body))){
		fprintf(stderr,"error: out of memory\n");
		free(repl->pending_name);
		repl->pending_name = NULL;
		repl->defining = false;
	}
	free(copy);
	return true;
}

static bool take_final_ret(char* input){
	char* p = input;
	char* last = NULL;
	size_t last_len = 0;
	while(*p){
		size_t whitespace;
		while((whitespace = frontend_whitespace_len(p))) p += whitespace;
		if(!*p) break;
		last = p;
		if(*p == '(' || *p == ')') p++;
		else while(*p && !frontend_whitespace_len(p) && *p != '(' && *p != ')') p++;
		last_len = (size_t)(p - last);
	}
	if(!last || last_len != 3 || memcmp(last,"Ret",3) != 0) return false;
	*last = 0;
	return true;
}

static void repl_free(Repl* repl){
	vm_func_s_free(&repl->ctx.code);
	for(size_t i=0;i<repl->ctx.funcs.len;i++){
		Frontend* fe = repl->frontends.data[i];
		free_built_func(fe,&repl->ctx.funcs.data[i]);
		free(fe);
	}
	for(size_t i=0;i<repl->names.len;i++) free(repl->names.data[i]);
	free(repl->ctx.funcs.data);
	free(repl->frontends.data);
	free(repl->names.data);
	free(repl->source.data);
	free(repl->pending_name);
	free(repl->ctx.globals.data);
	vm_free(&repl->vm);
	comp_context_free(&repl->ctx);
}

int main(void){
	Repl repl = {0};
	repl.ctx.globals.data = malloc(sizeof(*repl.ctx.globals.data));
	if(!repl.ctx.globals.data){
		fprintf(stderr,"error: out of memory\n");
		return 1;
	}
	repl.ctx.globals.data[0] = (Global){
		.var = {.tid = 2,.name = "Print"},
		.mem = &print_int_fn,
	};
	repl.ctx.globals.len = 1;
	repl.ctx.globals.cap = 1;
	char* line = NULL;
	size_t line_cap = 0;
	bool interactive = isatty(STDIN_FILENO);
	if(interactive) puts("PL-stuff REPL: Func Name defines; Ret completes; :quit exits");

	for(;;){
		if(interactive){
			fputs(repl.source.len || repl.defining ? "...> " : "pl> ",stdout);
			fflush(stdout);
		}
		ssize_t got = getline(&line,&line_cap,stdin);
		if(got < 0) break;
		char* input = trim(line);
		if(strcmp(input,":quit") == 0) break;
		bool complete = take_final_ret(input);
		input = trim(input);
		bool handled = !repl.defining && repl.source.len == 0 && begin_definition(&repl,input);
		if(!handled && input[0] && !append_source(&repl,input)){
			fprintf(stderr,"error: out of memory\n");
			break;
		}
		if(complete && (repl.defining || repl.source.len || !input[0])) finish_form(&repl);
	}

	if(repl.defining || repl.source.len) fprintf(stderr,"error: incomplete function at end of input; expected Ret\n");
	free(line);
	repl_free(&repl);
	return 0;
}
