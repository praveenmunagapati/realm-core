/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <cstdint>
#include <cassert>
#include <cstring>

#include "table.hpp"
#include "cuckoo.hpp"
#include "payload.hpp"
#include "array.hpp"
#include "snapshot_impl.hpp"

union ArrayReps {
    _Array<uint64_t> as_u;
    _Array<int64_t> as_i;
    _Array<float> as_f;
    _Array<double> as_d;
};

template<typename T> T get(Memory&, ArrayReps&, int);
template<> uint64_t get<uint64_t>(Memory& mem, ArrayReps& ar, int index) { return ar.as_u.get(mem, index); }
template<> int64_t get<int64_t>(Memory& mem, ArrayReps& ar, int index) { return ar.as_i.get(mem, index); }
template<> float get<float>(Memory& mem, ArrayReps& ar, int index) { return ar.as_f.get(mem, index); }
template<> double get<double>(Memory& mem, ArrayReps& ar, int index) { return ar.as_d.get(mem, index); }

template<typename T> void set(Memory&, ArrayReps&, int, T);
template<> void set(Memory& mem, ArrayReps& ar, int index, uint64_t val) { ar.as_u.set(mem, index, val); }
template<> void set(Memory& mem, ArrayReps& ar, int index, int64_t val) { ar.as_i.set(mem, index, val); }
template<> void set(Memory& mem, ArrayReps& ar, int index, float val) { ar.as_f.set(mem, index, val); }
template<> void set(Memory& mem, ArrayReps& ar, int index, double val) { ar.as_d.set(mem, index, val); }

union Reps {
    uint64_t as_u;
    int64_t as_i;
    float as_f;
    double as_d;
};

struct _Cluster { ArrayReps entries[1]; };

struct ClusterMgr : public PayloadMgr {
    ClusterMgr(Memory& mem, int num_fields, const char* typeinfo) 
        : mem(mem), num_fields(num_fields), typeinfo(typeinfo) {};
    virtual void cow(Ref<DynType>& payload, int old_capacity, int new_capacity);
    virtual void free(Ref<DynType> payload, int capacity);
    virtual void read_internalbuffer(Ref<DynType> payload, int from);
    virtual void write_internalbuffer(Ref<DynType>& payload, int to);
    virtual void init_internalbuffer();
    virtual void swap_internalbuffer(Ref<DynType>& payload, int index);
    virtual Ref<DynType> commit(Ref<DynType> from);
    Memory& mem;
    int num_fields;
    const char* typeinfo;
    Reps values[16];
};


void ClusterMgr::init_internalbuffer() {
    for (int j = 0; j < num_fields; ++j) values[j].as_u = 0;
}

void ClusterMgr::free(Ref<DynType> payload, int capacity) {
    if (!is_null(payload)) {
        Ref<_Cluster> cluster = payload.as<_Cluster>();
        _Cluster* cluster_ptr = mem.txl(cluster);
        for (int j=0; j < num_fields; j++) {
            switch(typeinfo[j]) {
                case 't': 
                case 'r':
                case 'u': cluster_ptr->entries[j].as_u.free(mem); break;
                case 'i': cluster_ptr->entries[j].as_i.free(mem); break;
                case 'f': cluster_ptr->entries[j].as_f.free(mem); break;
                case 'd': cluster_ptr->entries[j].as_d.free(mem); break;
                default: throw std::runtime_error("Internal error, unsupported type specifier");
            }
        }
        mem.free(payload, num_fields * sizeof(uint64_t));
    }
}

// FIXME: if old_capacity/new_capacity is unused here, then get rid of them!
void ClusterMgr::cow(Ref<DynType>& payload, int old_capacity, int new_capacity) {
    if (!mem.is_writable(payload) || new_capacity != old_capacity) {
        _Cluster* payload_ptr;
        assert(new_capacity != 0);
        assert(old_capacity <= 256);
        Ref<_Cluster> new_payload = mem.alloc<_Cluster>(payload_ptr, num_fields * sizeof(uint64_t));
        Ref<_Cluster> old_payload = payload.as<_Cluster>();
        _Cluster* old_payload_ptr = mem.txl(old_payload);
        for (int k = 0; k < num_fields; ++k) {
            payload_ptr->entries[k] = old_payload_ptr->entries[k];
        }
        mem.free(old_payload, num_fields * sizeof(uint64_t));
        payload = new_payload;
    }
}

Ref<DynType> ClusterMgr::commit(Ref<DynType> from) {
    if (mem.is_writable(from)) {
        _Cluster* from_ptr = mem.txl(from.as<_Cluster>());
        _Cluster* to_ptr;
        Ref<_Cluster> to = mem.alloc_in_file<_Cluster>(to_ptr, num_fields * sizeof(uint64_t));
        for (int k = 0; k < num_fields; ++k) {
            switch(typeinfo[k]) {
                case 't': 
                case 'r':
                case 'u': to_ptr->entries[k].as_u = _Array<uint64_t>::commit(mem, from_ptr->entries[k].as_u); break;
                case 'i': to_ptr->entries[k].as_i = _Array<int64_t>::commit(mem, from_ptr->entries[k].as_i); break;
                case 'f': to_ptr->entries[k].as_f = _Array<float>::commit(mem, from_ptr->entries[k].as_f); break;
                case 'd': to_ptr->entries[k].as_d = _Array<double>::commit(mem, from_ptr->entries[k].as_d); break;
                default: throw std::runtime_error("Internal error, unsupported type specifier");
            }
        }
        mem.free(from, num_fields * sizeof(uint64_t));
        return to;
    }
    return from;
}

void ClusterMgr::read_internalbuffer(Ref<DynType> payload, int index) {
    Ref<_Cluster> p_ref = payload.as<_Cluster>();
    _Cluster* p_ptr = mem.txl(p_ref);
    for (int col = 0; col < num_fields; ++col) {
        switch (typeinfo[col]) {
            case 't': 
            case 'r':
            case 'u': values[col].as_u = p_ptr->entries[col].as_u.get(mem, index); break;
            case 'i': values[col].as_i = p_ptr->entries[col].as_i.get(mem, index); break;
            case 'f': values[col].as_f = p_ptr->entries[col].as_f.get(mem, index); break;
            case 'd': values[col].as_d = p_ptr->entries[col].as_d.get(mem, index); break;
            default: throw std::runtime_error("Internal error, unsupported type specifier");
        }
    }
}

void ClusterMgr::write_internalbuffer(Ref<DynType>& payload, int index) {
    assert(mem.is_writable(payload));
    Ref<_Cluster> p_ref = payload.as<_Cluster>();
    _Cluster* p_ptr = mem.txl(p_ref);
    for (int col = 0; col < num_fields; ++col) {
        switch (typeinfo[col]) {
            case 't': 
            case 'r':
            case 'u': p_ptr->entries[col].as_u.set(mem, index, values[col].as_u); break;
            case 'i': p_ptr->entries[col].as_i.set(mem, index, values[col].as_i); break;
            case 'f': p_ptr->entries[col].as_f.set(mem, index, values[col].as_f); break;
            case 'd': p_ptr->entries[col].as_d.set(mem, index, values[col].as_d); break;
            default: throw std::runtime_error("Internal error, unsupported type specifier");
        }
    }
}

void ClusterMgr::swap_internalbuffer(Ref<DynType>& payload, int index) {
    assert(mem.is_writable(payload));
    Ref<_Cluster> p_ref = payload.as<_Cluster>();
    _Cluster* p_ptr = mem.txl(p_ref);
    for (int col = 0; col < num_fields; ++col) {
        switch (typeinfo[col]) {
            case 't': 
            case 'r':
            case 'u': {
                _Array<uint64_t>& array = p_ptr->entries[col].as_u;
                uint64_t tmp = array.get(mem, index);
                p_ptr->entries[col].as_u.set(mem, index, values[col].as_u);
                values[col].as_u = tmp;
                break;
            }
            case 'i': {
                _Array<int64_t>& array = p_ptr->entries[col].as_i;
                int64_t tmp = array.get(mem, index);
                p_ptr->entries[col].as_i.set(mem, index, values[col].as_i);
                values[col].as_i = tmp;
                break;
            }
            case 'f': {
                _Array<float>& array = p_ptr->entries[col].as_f;
                float tmp = array.get(mem, index);
                p_ptr->entries[col].as_f.set(mem, index, values[col].as_u);
                values[col].as_f = tmp;
                break;
            }
            case 'd': {
                _Array<double>& array = p_ptr->entries[col].as_d;
                double tmp = array.get(mem, index);
                p_ptr->entries[col].as_d.set(mem, index, values[col].as_d);
                values[col].as_d = tmp;
                break;
            }
            default: throw std::runtime_error("Internal error, unsupported type specifier");
        }
    }
}

Ref<_Table> _Table::cow(Memory& mem, Ref<_Table> from) {
    if (!mem.is_writable(from)) {
        _Table* to_ptr;
        Ref<_Table> to = mem.alloc<_Table>(to_ptr);
        _Table* from_ptr = mem.txl(from);
        *to_ptr = *from_ptr;
        mem.free(from);
        return to;
    }
    return from;
}

void _Table::copied_from_file(Memory& mem) {} // does nothing, could forward to cuckoo?

Ref<_Table> _Table::commit(Memory& mem, Ref<_Table> from) {
    if (mem.is_writable(from)) {
        _Table* to_ptr;
        Ref<_Table> to = mem.alloc_in_file<_Table>(to_ptr);
        _Table* from_ptr = mem.txl(from);
        *to_ptr = *from_ptr;
        mem.free(from);
        ClusterMgr pm(mem,to_ptr->num_fields, to_ptr->typeinfo);
        to_ptr->cuckoo.copied_to_file(mem, pm);
        return to;
    }
    return from;
}

void _Table::copied_to_file(Memory& mem) {
    ClusterMgr pm(mem, num_fields, typeinfo);
    cuckoo.copied_to_file(mem, pm);
}

void _Table::insert(Memory& mem, uint64_t key) {
    ClusterMgr pm(mem, num_fields, typeinfo);
    pm.init_internalbuffer();
    cuckoo.insert(mem, key << 1, pm);
}

void _Table::get_cluster(Memory& mem, uint64_t key, Object& o) {
    Ref<DynType> payload;
    int index;
    if (cuckoo.find(mem, key, payload, index)) {
        Ref<_Cluster> pl = payload.as<_Cluster>();
        _Cluster* pl_ptr = mem.txl(pl);
        o.cluster = pl_ptr;
        o.index = index;
        o.is_writable = mem.is_writable(pl);
        return;
    }
    throw NotFound();
}

void _Table::change_cluster(Memory& mem, uint64_t key, Object& o) {
    ClusterMgr pm(mem, num_fields, typeinfo);
    Ref<DynType> payload;
    int index;
    bool res = cuckoo.find_and_cow_path(mem, pm, key, payload, index);
    if (!res) {
        throw NotFound();
    }
    assert(mem.is_writable(payload));
    Ref<_Cluster> pl = payload.as<_Cluster>();
    _Cluster* pl_ptr = mem.txl(pl);
    o.cluster = pl_ptr;
    o.index = index;
    o.is_writable = true;
}

bool _Table::find(Memory& mem, uint64_t key) {
    int dummy;
    Ref<DynType> dot;
    return cuckoo.find(mem, key, dot, dummy);
}

void _Table::init(const char* t_info) {
    num_fields = strlen(t_info);
    for (int j = 0; j < num_fields; ++j)
        typeinfo[j] = t_info[j];
    cuckoo.init();
}

bool _Table::first_access(Memory& mem, ObjectIterator& oi) {
    // idx -> leaf,payload,num_elems
    // leaf,elem -> key
    return cuckoo.first_access(mem, oi);
}




template<typename T>
void Object::set(Field<T> f, T value) {
    Memory& mem = ss->change(this);
    ::set<T>(mem, cluster->entries[f.key], index, value);
}

template<>
void Object::set<Table>(Field<Table> f, Table value) {
    Memory& mem = ss->change(this);
    ::set<uint64_t>(mem, cluster->entries[f.key], index, value.key);
}

template<>
void Object::set<Row>(Field<Row> f, Row value) {
    Memory& mem = ss->change(this);
    ::set<uint64_t>(mem, cluster->entries[f.key], index, value.key);
}

template<typename T>
T Object::operator()(Field<T> f) {
    Memory& mem = ss->refresh(this);
    return ::get<T>(mem, cluster->entries[f.key], index);
}

template<>
Table Object::operator()<Table>(Field<Table> f) {
    Memory& mem = ss->refresh(this);
    Table res;
    res.key = ::get<uint64_t>(mem, cluster->entries[f.key], index);
    return res;
}

template<>
Row Object::operator()<Row>(Field<Row> f) {
    Memory& mem = ss->refresh(this);
    Row res;
    res.key = ::get<uint64_t>(mem, cluster->entries[f.key], index);
    return res;
}

// explicit instantiation
template void Object::set<uint64_t>(Field<uint64_t>, uint64_t);
template void Object::set<int64_t>(Field<int64_t>, int64_t);
template void Object::set<float>(Field<float>, float);
template void Object::set<double>(Field<double>, double);
template void Object::set<Table>(Field<Table>, Table);
template void Object::set<Row>(Field<Row>, Row);

template uint64_t Object::operator()<uint64_t>(Field<uint64_t>);
template int64_t Object::operator()<int64_t>(Field<int64_t>);
template float Object::operator()<float>(Field<float>);
template double Object::operator()<double>(Field<double>);
template Table Object::operator()<Table>(Field<Table>);
template Row Object::operator()<Row>(Field<Row>);