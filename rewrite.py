import re

with open('src/handlers.c', 'r') as f:
    lines = f.readlines()

# Define blocks (start_line is 1-indexed, inclusive)
blocks = {
    'includes': (1, 26),
    'fwd_decls': (27, 30),
    'build_sysinfo': (69, 103),
    'append_remote': (169, 181),
    'shared_utils': (459, 497),
    'login_utils': (512, 576),
    'upload_utils': (730, 743)
}

def get_lines(block_name):
    start, end = blocks[block_name]
    return lines[start-1:end]

def remove_blocks(lines_list, blocks_to_remove):
    # Sort blocks in reverse order so deleting doesn't shift indices
    sorted_blocks = sorted(blocks_to_remove, key=lambda x: x[0], reverse=True)
    new_lines = lines_list[:]
    for start, end in sorted_blocks:
        del new_lines[start-1:end]
    return new_lines

# Build the new content
new_content = []

# 1. Includes
includes = get_lines('includes')
# Remove dead includes as per report
dead_includes = ['<assert.h>', '<time.h>', '<unistd.h>', '<event2/keyvalq_struct.h>']
for line in includes:
    if not any(dead in line for dead in dead_includes):
        new_content.append(line)

new_content.append('\n// ==============================================================================\n')
new_content.append('// UTILITY FUNCTIONS & SHARED CONTEXT\n')
new_content.append('// ==============================================================================\n\n')

# 2. Add all utility blocks
new_content.extend(get_lines('shared_utils'))
new_content.append('\n')
new_content.extend(get_lines('build_sysinfo'))
new_content.append('\n')
new_content.extend(get_lines('append_remote'))
new_content.append('\n')
new_content.extend(get_lines('login_utils'))
new_content.append('\n')
new_content.extend(get_lines('upload_utils'))
new_content.append('\n')

new_content.append('\n// ==============================================================================\n')
new_content.append('// HTTP HANDLERS\n')
new_content.append('// ==============================================================================\n\n')

# 3. Add the rest of the file (excluding removed blocks)
blocks_to_remove = [
    blocks['includes'],
    blocks['fwd_decls'],
    blocks['build_sysinfo'],
    blocks['append_remote'],
    blocks['shared_utils'],
    blocks['login_utils'],
    blocks['upload_utils']
]
remaining_lines = remove_blocks(lines, blocks_to_remove)

# Also fix the section comment at line 354
for i, line in enumerate(remaining_lines):
    if '// --- TOTP QR Handler ---' in line:
        remaining_lines[i] = line.replace('// --- TOTP QR Handler ---', '// --- Shippers & Products Handlers ---')
    # Also fix CustomersSchema max_len if we want (wait, we need to be careful. The user didn't explicitly ask, but it's part of the cleanup. I'll leave max_len=5 alone for now as it might be intentional.)

new_content.extend(remaining_lines)

with open('src/handlers.c', 'w') as f:
    f.writelines(new_content)
