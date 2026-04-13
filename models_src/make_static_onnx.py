import argparse
import onnx
from onnx import shape_inference


def fix_first_dim_to_one(value_info):
    if not value_info.type.HasField("tensor_type"):
        return
    shape = value_info.type.tensor_type.shape
    if len(shape.dim) == 0:
        return
    dim0 = shape.dim[0]
    if dim0.HasField("dim_param") or (not dim0.HasField("dim_value")):
        dim0.ClearField("dim_param")
        dim0.dim_value = 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, help="input onnx")
    parser.add_argument("--output", required=True, help="output static onnx")
    args = parser.parse_args()

    model = onnx.load(args.input)

    for x in model.graph.input:
        fix_first_dim_to_one(x)

    for x in model.graph.output:
        fix_first_dim_to_one(x)

    for x in model.graph.value_info:
        fix_first_dim_to_one(x)

    model = shape_inference.infer_shapes(model)
    onnx.checker.check_model(model)
    onnx.save(model, args.output)

    print(f"[OK] saved static onnx: {args.output}")


if __name__ == "__main__":
    main()