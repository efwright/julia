@testset "large filter" begin
    n = 12000000
    k = 257000000
    vals = vcat(fill(0.5, n), fill(1.5, k))
    vals_filt = filter(x -> x[2] < 1.0, collect(enumerate(vals)))
    @test vals_filt[end] == 0.5
end

