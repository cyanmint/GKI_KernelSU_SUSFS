#!/bin/bash
# Script to apply CONFIG_CGROUP_DEVICE patch to kernel/cgroup/cgroup.c
# This patch adds support for cgroup device controller to avoid bootloop

set -e

CGROUP_FILE="kernel/cgroup/cgroup.c"

if [ ! -f "$CGROUP_FILE" ]; then
    echo "Error: $CGROUP_FILE not found!"
    exit 1
fi

echo "Applying CONFIG_CGROUP_DEVICE patch to $CGROUP_FILE..."

# Find the line number of the cgroup_add_file function
# Look for the function - it might span multiple lines
FUNC_LINE=$(awk '/^static int cgroup_add_file/ {print NR; exit}' "$CGROUP_FILE")

if [ -z "$FUNC_LINE" ]; then
    echo "Error: Could not find cgroup_add_file function!"
    exit 1
fi

echo "Found cgroup_add_file function at line $FUNC_LINE"

# Find the closing brace of the specific code block we need to patch after
# We're looking for the block that ends with:
#     }
# after the lines:
#     cfile->kn = kn;
#     spin_unlock_irq(&cgroup_file_kn_lock);
#   }

# Search for the pattern starting from the function line
PATCH_LINE=$(awk -v start="$FUNC_LINE" '
NR >= start {
    if ($0 ~ /cfile->kn = kn;/) {
        found_kn = NR
    }
    if (found_kn && $0 ~ /spin_unlock_irq\(&cgroup_file_kn_lock\);/) {
        found_unlock = NR
    }
    if (found_unlock && $0 ~ /^[[:space:]]*}[[:space:]]*$/) {
        print NR
        exit
    }
}
' "$CGROUP_FILE")

if [ -z "$PATCH_LINE" ]; then
    echo "Error: Could not find the correct location to apply patch!"
    echo "Looking for the closing brace after 'cfile->kn = kn;' and 'spin_unlock_irq(&cgroup_file_kn_lock);'"
    exit 1
fi

echo "Will insert patch after line $PATCH_LINE"

# Check if patch is already applied
if grep -q "CGRP_ROOT_NOPREFIX" "$CGROUP_FILE"; then
    echo "Patch already applied, skipping..."
    exit 0
fi

# Apply the patch using sed (insert after the found line)
# We need to insert the new code block after the closing brace
sed -i "${PATCH_LINE} a\\
\\tif (cft->ss && (cgrp->root->flags & CGRP_ROOT_NOPREFIX) && !(cft->flags & CFTYPE_NO_PREFIX)) {\\
\\t\\t\\t\\tsnprintf(name, CGROUP_FILE_NAME_MAX, \"%s.%s\", cft->ss->name, cft->name);\\
\\t\\t\\t\\tkernfs_create_link(cgrp->kn, name, kn);\\
\\t}" "$CGROUP_FILE"

echo "Patch applied successfully!"
echo "Verifying patch..."

if grep -q "CGRP_ROOT_NOPREFIX" "$CGROUP_FILE"; then
    echo "Verification successful: Patch is present in the file"
    exit 0
else
    echo "Error: Patch verification failed!"
    exit 1
fi
