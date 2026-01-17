import os

# --- CONFIGURATION ---

# The Header for C++, Swift, Java, JS, TS (Block Style)
C_STYLE_HEADER = """/*
 * Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
 *
 * This source code is licensed under the terms found in the
 * LICENSE file in the root directory of this source tree.
 *
 * USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.
 */

"""

# The Header for Python, Shell, CMake (Hash Style)
PY_STYLE_HEADER = """# Copyright (c) 2026 Ayon Sarkar. All Rights Reserved.
#
# This source code is licensed under the terms found in the
# LICENSE file in the root directory of this source tree.
#
# USE FOR EVALUATION ONLY. NO PRODUCTION USE OR COPYING PERMITTED.

"""

# Map extensions to their correct header style
FILE_TYPES = {
    # C-Style Comments
    ".cpp": C_STYLE_HEADER,
    ".h": C_STYLE_HEADER,
    ".hpp": C_STYLE_HEADER,
    ".c": C_STYLE_HEADER,
    ".swift": C_STYLE_HEADER,
    ".ts": C_STYLE_HEADER,
    ".js": C_STYLE_HEADER,
    ".java": C_STYLE_HEADER,
    ".go": C_STYLE_HEADER,
    
    # Hash-Style Comments
    ".py": PY_STYLE_HEADER,
    ".sh": PY_STYLE_HEADER,
    ".cmake": PY_STYLE_HEADER,
    "CMakeLists.txt": PY_STYLE_HEADER
}

# Folders to explicitly ignore
IGNORE_DIRS = {".git", "build", "node_modules", ".vs", ".vscode", "dist", "target","vcpkg_installed"}

# --- MAIN SCRIPT ---

def add_header():
    print("🚀 Starting header injection...")
    count = 0
    
    for root, dirs, files in os.walk("."):
        # Modify dirs in-place to skip ignored folders
        dirs[:] = [d for d in dirs if d not in IGNORE_DIRS]
        
        for file in files:
            # Check if file has a valid extension
            ext = os.path.splitext(file)[1]
            header_to_use = None

            # Special case for CMakeLists.txt
            if file == "CMakeLists.txt":
                header_to_use = PY_STYLE_HEADER
            elif ext in FILE_TYPES:
                header_to_use = FILE_TYPES[ext]
            
            if header_to_use:
                path = os.path.join(root, file)
                
                try:
                    with open(path, "r", encoding="utf-8") as f:
                        content = f.read()
                        
                    # SAFETY CHECK: Don't add if it already exists
                    if "Copyright (c) 2026 Ayon Sarkar" in content:
                        print(f"   [SKIP] Already exists: {file}")
                        continue
                        
                    # Write the header + original content
                    with open(path, "w", encoding="utf-8") as f:
                        f.write(header_to_use + content)
                        print(f"✅ [DONE] Added to: {file}")
                        count += 1
                        
                except Exception as e:
                    print(f"❌ [ERROR] Could not process {file}: {e}")

    print(f"\n🎉 Finished! Added headers to {count} files.")

if __name__ == "__main__":
    add_header()