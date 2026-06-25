#pragma once
#include <string>
#include <map>
#include <fstream>
#include <optional>
#include <cstdint>
#include <stdexcept>
class WalStore {
public:
    explicit WalStore(const std::string& wal_path) : wal_path_(wal_path) {
        recover();
        wal_.open(wal_path_, std::ios::binary | std::ios::app);
        if (!wal_) throw std::runtime_error("cannot open WAL: " + wal_path_);
    }
    void put(const std::string& key, const std::string& value) { append_put(key,value); mem_[key]=value; }
    void del(const std::string& key) { append_del(key); mem_.erase(key); }
    std::optional<std::string> get(const std::string& key) const {
        auto it=mem_.find(key); if(it==mem_.end()) return std::nullopt; return it->second;
    }
    size_t size() const { return mem_.size(); }
    const std::map<std::string,std::string>& mem() const { return mem_; }
private:
    std::map<std::string,std::string> mem_;
    std::ofstream wal_; std::string wal_path_;
    static constexpr uint8_t OP_PUT=1, OP_DEL=2;
    void write_u32(std::ostream& os,uint32_t v){os.write(reinterpret_cast<const char*>(&v),4);}
    static bool read_u32(std::istream& is,uint32_t& v){return static_cast<bool>(is.read(reinterpret_cast<char*>(&v),4));}
    void append_put(const std::string& k,const std::string& v){
        wal_.put(OP_PUT); write_u32(wal_,k.size()); wal_.write(k.data(),k.size());
        write_u32(wal_,v.size()); wal_.write(v.data(),v.size()); wal_.flush();
    }
    void append_del(const std::string& k){
        wal_.put(OP_DEL); write_u32(wal_,k.size()); wal_.write(k.data(),k.size()); wal_.flush();
    }
    void recover(){
        std::ifstream in(wal_path_,std::ios::binary); if(!in) return;
        int n=0;
        while(in.peek()!=EOF){
            int op=in.get(); uint32_t klen; if(!read_u32(in,klen)) break;
            std::string key(klen,'\0'); if(!in.read(&key[0],klen)) break;
            if(op==OP_PUT){uint32_t vlen; if(!read_u32(in,vlen)) break;
                std::string val(vlen,'\0'); if(!in.read(&val[0],vlen)) break; mem_[key]=val;}
            else if(op==OP_DEL) { mem_.erase(key); }
            else { break; }
            ++n;
        }
        if(n) std::fprintf(stderr,"[recover] replayed %d WAL records\n",n);
    }
};
