from pathlib import Path
root=Path.cwd(); work=Path('/tmp/shadps4-g2-close-review-20260909')
s=(root/'tests/guest_cpu/guest_execution_tests.cpp').read_text()
assert s.count('FindFixture("smc_writer")') == 1
s=s.replace('FindFixture("smc_writer")','FindFixture("return_only")')
(work/'guest_store_negative.cpp').write_text('#define main full_suite_main\n'+s+'\n#undef main\nint main() {\n#include "setup.inc"\nTestGuestStorePublication(harness);\nreturn g_failures ? 1 : 0;\n}\n')
p=work/'CMakeLists.txt';cm=p.read_text()
if 'add_executable(guest_store_negative' not in cm:
    cm+='\nadd_executable(guest_store_negative guest_store_negative.cpp)\ntarget_link_libraries(guest_store_negative PRIVATE guest_cpu_fex)\ntarget_include_directories(guest_store_negative PRIVATE "${V0_FIXTURE_DIR}")\ntarget_link_options(guest_store_negative PRIVATE -Wl,--build-id=sha1)\nadd_dependencies(guest_store_negative guest_execution_tests)\n'
    p.write_text(cm)
