#include "front_end.h"

static bool par_eq(Par a, Par b){
	return a.kind == b.kind && a.idx == b.idx && a.parent == b.parent;
}

static size_t par_hash(Par par){
	size_t h = 1469598103934665603ull;
	h = (h ^ (size_t)par.kind) * 1099511628211ull;
	h = (h ^ (size_t)par.idx) * 1099511628211ull;
	h = (h ^ (size_t)par.parent) * 1099511628211ull;
	return h;
}

void par_idx_table_free(ParIdxTable* table){
	free(table->keys);
	free(table->vals);
	free(table->used);
	*table = (ParIdxTable){0};
}

static void par_idx_table_grow(ParIdxTable* table){
	ParIdxTable old = *table;
	size_t cap = old.cap ? old.cap * 2 : 16;
	table->keys = calloc(cap, sizeof(*table->keys));
	table->vals = calloc(cap, sizeof(*table->vals));
	table->used = calloc(cap, sizeof(*table->used));
	assert(table->keys && table->vals && table->used);
	table->len = 0;
	table->cap = cap;

	for(size_t i = 0; i < old.cap; i++){
		if(!old.used[i]) continue;
		size_t j = par_hash(old.keys[i]) & (table->cap - 1);
		while(table->used[j]) j = (j + 1) & (table->cap - 1);
		table->keys[j] = old.keys[i];
		table->vals[j] = old.vals[i];
		table->used[j] = 1;
		table->len++;
	}

	free(old.keys);
	free(old.vals);
	free(old.used);
}

bool par_idx_table_get(const ParIdxTable* table, Par key, par_idx* out){
	if(!table->cap) return false;
	size_t i = par_hash(key) & (table->cap - 1);
	while(table->used[i]){
		if(par_eq(table->keys[i], key)){
			if(out) *out = table->vals[i];
			return true;
		}
		i = (i + 1) & (table->cap - 1);
	}
	return false;
}

void par_idx_table_put(ParIdxTable* table, Par key, par_idx val){
	if((table->len + 1) * 4 >= table->cap * 3) par_idx_table_grow(table);
	size_t i = par_hash(key) & (table->cap - 1);
	while(table->used[i]){
		if(par_eq(table->keys[i], key)){
			table->vals[i] = val;
			return;
		}
		i = (i + 1) & (table->cap - 1);
	}
	table->keys[i] = key;
	table->vals[i] = val;
	table->used[i] = 1;
	table->len++;
}

par_idx comp_context_intern_par(CompileContext* ctx, Par par){
	par_idx idx = PAR_IDX_INVALID;
	if(par_idx_table_get(&ctx->par_idxs, par, &idx)) return idx;
	assert(ctx->handles.len == (size_t)(par_idx)ctx->handles.len);
	idx = (par_idx)ctx->handles.len;
	PUSH_HEAP(ctx->handles, ((Handle){.par = par}));
	par_idx_table_put(&ctx->par_idxs, par, idx);
	return idx;
}
