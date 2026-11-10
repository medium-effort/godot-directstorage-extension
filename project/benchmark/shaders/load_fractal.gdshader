shader_type canvas_item;

// Synthetic GPU load for bench_gpu_contention.gd. The point is not the image:
// it is a controllable amount of fragment work so streaming can be measured
// while the GPU is already busy (the sample's Mandelbrot idea).
// `iterations` is the workload dial; 0 never reaches the loop body.

uniform int iterations = 120;
uniform vec4 tint : source_color = vec4(0.15, 0.35, 0.65, 1.0);

void fragment() {
	vec2 c = (UV - vec2(0.5)) * 3.0;
	vec2 z = vec2(0.0);
	float steps = 0.0;
	for (int i = 0; i < 2048; i++) {
		if (i >= iterations) {
			break;
		}
		z = vec2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
		if (dot(z, z) > 4.0) {
			break;
		}
		steps += 1.0;
	}
	float t = clamp(steps / float(max(iterations, 1)), 0.0, 1.0);
	COLOR = vec4(t * tint.rgb + vec3(0.02, 0.02, 0.02), 1.0);
}
