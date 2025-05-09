import binascii

def convert_tflite_to_header(tflite_path, header_path):
    with open(tflite_path, 'rb') as f:
        content = f.read()
    
    hex_str = binascii.hexlify(content).decode('utf-8')
    hex_lines = [hex_str[i:i+32] for i in range(0, len(hex_str), 32)]
    
    with open(header_path, 'w') as f:
        f.write('#ifndef MODEL_H\n')
        f.write('#define MODEL_H\n\n')
        f.write('const unsigned char model_tflite[] = {\n')
        
        for line in hex_lines:
            hex_values = ', '.join(f'0x{line[i:i+2]}' for i in range(0, len(line), 2))
            f.write(f'    {hex_values},\n')
        
        f.write('};\n\n')
        f.write(f'const unsigned int model_tflite_len = {len(content)};\n\n')
        f.write('#endif // MODEL_H\n')

# Usage:
convert_tflite_to_header('/home/survivor/Downloads/model.tflite', 'model.h')