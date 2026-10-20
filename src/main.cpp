#include "block_store.hpp"

static void help() {
    std::cout << "CloudBlock Local Simulator 0.2 - not a Windows mounted disk\n"
              << "cloudblock ROOT [--cache-mib N] COMMAND ...\n"
              << "  status                    print cache/disk counters\n"
              << "  read OFFSET LENGTH        output hex (max 1MiB)\n"
              << "  write OFFSET HEX          write hex bytes\n"
              << "  put OFFSET INPUT_FILE     write a host file to virtual volume\n"
              << "  get OFFSET LENGTH OUTPUT  export virtual volume bytes\n"
              << "  flush                     persist all dirty cached chunks into disk/\n";
}
int main(int argc, char** argv) {
    try {
        if (argc < 3) { help(); return argc == 1 ? 0 : 2; }
        fs::path root(argv[1]);
        int a = 2; u64 cap = CAPACITY;
        if (std::string(argv[a]) == "--cache-mib") {
            check(a + 1 < argc, "missing cache capacity");
            u64 mb = parse(argv[a + 1]);
            check(mb <= std::numeric_limits<u64>::max() / MIB, "overflow");
            cap = mb * MIB; a += 2;
        }
        check(a < argc, "missing command");
        std::string cmd(argv[a++]);
        Store s(root, cap);
        if (cmd == "status") s.status();
        else if (cmd == "flush") { s.flush(); s.status(); }
        else if (cmd == "read") {
            check(a + 2 == argc, "read OFFSET LENGTH");
            u64 off = parse(argv[a++]), size = parse(argv[a++]);
            check(size <= BUFFER, "maximum hex read is 1MiB");
            std::vector<char> buf(static_cast<std::size_t>(size));
            s.read(off, buf.data(), buf.size());
            for (unsigned char v : buf)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << unsigned(v);
            std::cout << '\n';
        } else if (cmd == "write") {
            check(a + 2 == argc, "write OFFSET HEX");
            u64 off = parse(argv[a++]);
            std::string hex(argv[a++]);
            check(hex.size() % 2 == 0, "hex length must be even");
            std::vector<char> bytes; bytes.reserve(hex.size() / 2);
            for (std::size_t j = 0; j < hex.size(); j += 2) {
                std::size_t used = 0;
                auto v = std::stoul(hex.substr(j, 2), &used, 16);
                check(used == 2 && v <= 255, "invalid hex byte");
                bytes.push_back(static_cast<char>(v));
            }
            s.write(off, bytes.data(), bytes.size());
            std::cout << "written=" << bytes.size() << '\n';
        } else if (cmd == "put") {
            check(a + 2 == argc, "put OFFSET INPUT");
            u64 off = parse(argv[a++]);
            fs::path input(argv[a++]);
            u64 size = fs::file_size(input);
            range(off, size);
            std::ifstream in(input, std::ios::binary);
            check(bool(in), "cannot open input");
            std::vector<char> buf(BUFFER);
            u64 nbytes = 0;
            while (in) {
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                auto n = static_cast<std::size_t>(in.gcount());
                if (n) { s.write(off, buf.data(), n); off += n; nbytes += n; }
            }
            check(in.eof(), "input read failed");
            std::cout << "written=" << nbytes << '\n';
        } else if (cmd == "get") {
            check(a + 3 == argc, "get OFFSET LENGTH OUTPUT");
            u64 off = parse(argv[a++]), count = parse(argv[a++]);
            range(off, count);
            fs::path output(argv[a++]);
            std::ofstream out(output, std::ios::binary | std::ios::trunc);
            check(bool(out), "cannot open output");
            std::vector<char> buf(BUFFER);
            while (count) {
                auto n = static_cast<std::size_t>(std::min<u64>(count, BUFFER));
                s.read(off, buf.data(), n);
                out.write(buf.data(), static_cast<std::streamsize>(n));
                check(bool(out), "output write failed");
                off += n; count -= n;
            }
        } else { help(); return 2; }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
