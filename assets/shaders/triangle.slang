struct VertexInput {
    float3 position : POSITION;
    float3 color : COLOR;
};
struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR;
};
struct FrameConstants {
    float4x4 m_view;
    float4x4 m_projection;
    float4x4 m_view_projection;
    float4x4 m_inverse_view_projection;
    float4   m_camera_position;
    float4   m_viewport_size;
};
ConstantBuffer<FrameConstants> g_frame : register(b0);
[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    // mango::math matrices (lookat/perspectiveD3D/ortho) are row-vector convention:
    // translation lives in row 3 and Mango's perspectiveD3D produces positive
    // clip.w for +Z view-space points. Match that here with mul(row_vector, matrix)
    // instead of mul(matrix, column_vector), which would transpose the transform.
    output.position = mul(float4(input.position, 1.0), g_frame.m_view_projection);
    output.color = input.color;
    return output;
}
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
