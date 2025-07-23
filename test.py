import sys
from pathlib import Path


libs_path = str(Path(__file__).parent / "libs")
sys.path.append(libs_path)

pyd_file = Path(libs_path) / 'fastjson.pyd'
if not pyd_file.exists():
    raise FileNotFoundError(f"FastJSON library not found at {pyd_file}")

try:
    import fastjson

    data = {
        "name": "John Doe",
        "age": 30,
        "scores": [95, 87, 92],
        "active": True,
        "details": {"city": "New York", "zip": "10001"},
        "set_example": {1, 2, 3}
    }

    print("Testing dumps:")
    json_str = fastjson.dumps(data, indent=4)
    print(json_str)

    print("\nTesting encode:")
    json_str_encoded = fastjson.encode(data, indent=2)
    print(json_str_encoded)

    print("\nTesting loads:")
    parsed = fastjson.loads(json_str)
    print(parsed)

    print("\nTesting dump:")
    with open("output.json", "w") as f:
        fastjson.dump(data, f, indent=5)

    print("\nTesting load:")
    with open("output.json", "r") as f:
        loaded = fastjson.load(f)
    print(loaded)
except ImportError as e:
    print(f"Ошибка импорта: {e}")
