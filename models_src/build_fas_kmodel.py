import os
import glob
import argparse
import numpy as np
import nncase


def read_model_file(model_file):
    with open(model_file, "rb") as f:
        return f.read()


def load_calib_npy(dataset_dir, expected_shape=(1, 3, 128, 128), limit=32):
    files = sorted(glob.glob(os.path.join(dataset_dir, "*.npy")))
    if not files:
        raise RuntimeError(f"No .npy files found in {dataset_dir}")

    files = files[:limit]
    data = []
    for f in files:
        arr = np.load(f)
        if arr.shape != expected_shape:
            raise ValueError(f"{f}: shape={arr.shape}, expected={expected_shape}")
        if arr.dtype != np.float32:
            arr = arr.astype(np.float32)
        data.append([arr])  # 单输入模型
    return data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True, help="Path to static float ONNX model")
    parser.add_argument("--dataset", required=True, help="Directory of calibration .npy files")
    parser.add_argument("--output", required=True, help="Output kmodel path")
    parser.add_argument("--samples", type=int, default=32, help="Calibration sample count")
    parser.add_argument("--dump_dir", default="nncase_dump", help="Dump dir")
    args = parser.parse_args()

    os.makedirs(args.dump_dir, exist_ok=True)

    compile_options = nncase.CompileOptions()
    compile_options.target = "k230"
    compile_options.preprocess = False
    compile_options.dump_ir = True
    compile_options.dump_asm = False
    compile_options.dump_dir = args.dump_dir

    compiler = nncase.Compiler(compile_options)

    model_content = read_model_file(args.model)
    import_options = nncase.ImportOptions()
    compiler.import_onnx(model_content, import_options)

    cali_data = load_calib_npy(args.dataset, limit=args.samples)
    actual_samples = len(cali_data)
    print(f"[INFO] requested samples={args.samples}, actual samples={actual_samples}")

    ptq_options = nncase.PTQTensorOptions()
    ptq_options.samples_count = actual_samples
    ptq_options.set_tensor_data(cali_data)

    compiler.use_ptq(ptq_options)
    compiler.compile()

    kmodel = compiler.gencode_tobytes()
    with open(args.output, "wb") as f:
        f.write(kmodel)

    print(f"[OK] saved: {args.output}")


if __name__ == "__main__":
    main()