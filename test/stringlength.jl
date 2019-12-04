@inline function length_continued_test(s::String, i::Int, n::Int, c::Int)
    i < n || return c
    @inbounds b = codeunit(s, i)
    @inbounds while true
        while true
            (i += 1) ≤ n || return c
            0xc0 ≤ b ≤ 0xf7 && break
            b = codeunit(s, i)
        end
        l = b
        b = codeunit(s, i) # cont byte 1
        c -= (x = b & 0xc0 == 0x80)
        x & (l ≥ 0xe0) || continue

        (i += 1) ≤ n || return c
        b = codeunit(s, i) # cont byte 2
        c -= (x = b & 0xc0 == 0x80)
        x & (l ≥ 0xf0) || continue

        (i += 1) ≤ n || return c
        b = codeunit(s, i) # cont byte 3
        c -= (b & 0xc0 == 0x80)
    end
end
length_test(s::String) = length_continued_test(s, 1, ncodeunits(s), ncodeunits(s))

buffer = Vector{UInt8}(undef, 4)

@testset "string length with ill-formed UTF-8" begin
    for i in 0x00:0xFF
        for j in 0x00:0xFF
            buffer[1] = i
            buffer[2] = j
            buffer[3] = j
            buffer[4] = i
            s = unsafe_string(pointer(buffer), 4)
            @test length_test(s) == length(s)
        end
    end
end

