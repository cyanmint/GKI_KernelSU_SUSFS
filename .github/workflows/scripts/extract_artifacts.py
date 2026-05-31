#!/usr/bin/env python3
import os
import zipfile
import shutil
import argparse


def extract_artifacts(artifacts_dir: str, output_dir: str):
    """从 artifact zip 中提取 android*boot*.img 和 AnyKernel3 zip 文件"""
    
    for zipfile_path in os.listdir(artifacts_dir):
        if not zipfile_path.endswith('.zip'):
            continue
        
        full_path = os.path.join(artifacts_dir, zipfile_path)
        
        try:
            with zipfile.ZipFile(full_path, 'r') as zf:
                for member in zf.namelist():
                    try:
                        if member.startswith('__MACOSX/'):
                            continue
                        filename = os.path.basename(member) if '/' in member else member
                        # 提取 android*boot*.img 和 android*AnyKernel*.zip
                        if filename.startswith('android') and ('boot' in filename or 'AnyKernel3' in filename):
                            target_path = os.path.join(output_dir, filename)
                            print(f"提取: {filename}")
                            with zf.open(member) as src, open(target_path, 'wb') as dst:
                                shutil.copyfileobj(src, dst)
                    except Exception:
                        pass
        except zipfile.BadZipFile:
            print(f"警告: 无效 zip - {zipfile_path}")
        except Exception as e:
            print(f"错误: {e}")
    
    print("完成")


def main():
    parser = argparse.ArgumentParser(description="提取 artifact 文件")
    parser.add_argument("artifacts_dir", help="artifact 目录路径")
    parser.add_argument("output_dir", help="输出目录路径")
    args = parser.parse_args()
    
    extract_artifacts(args.artifacts_dir, args.output_dir)


if __name__ == "__main__":
    main()
