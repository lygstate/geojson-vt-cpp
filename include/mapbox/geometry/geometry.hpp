#pragma once

#include <mapbox/geometry/empty.hpp>
#include <mapbox/geometry/point.hpp>
#include <mapbox/geometry/line_string.hpp>
#include <mapbox/geometry/polygon.hpp>
#include <mapbox/geometry/multi_point.hpp>
#include <mapbox/geometry/multi_line_string.hpp>
#include <mapbox/geometry/multi_polygon.hpp>

#include <mapbox/variant.hpp>

// stl
#include <vector>

namespace mapbox {
namespace geometry {

template <typename T, template <typename...> class Cont = std::vector>
struct geometry_collection;

template <typename T, template <typename...> class Cont = std::vector>
using geometry_base = mapbox::util::variant<empty,
                                            point<T>,
                                            line_string<T, Cont>,
                                            polygon<T, Cont>,
                                            multi_point<T, Cont>,
                                            multi_line_string<T, Cont>,
                                            multi_polygon<T, Cont>,
                                            geometry_collection<T, Cont>>;

template <typename T, template <typename...> class Cont = std::vector>
struct geometry : geometry_base<T, Cont>
{
    using coordinate_type = T;
    using geometry_base_type = geometry_base<T>;

    template <typename U>
    geometry(const U &_v) :geometry_base_type(_v) {}
    template <typename U>
    geometry(U&& _v) :geometry_base_type(_v) {}

    /*
     * The default constructor would create a point geometry with default-constructed coordinates;
     * i.e. (0, 0). Since this is not particularly useful, and could hide bugs, it is disabled.
     */
    geometry() = delete;
};

template <typename T, template <typename...> class Cont>
struct geometry_collection : Cont<geometry<T>>
{
    using coordinate_type = T;
    using geometry_type = geometry<T>;
    using container_type = Cont<geometry_type>;
    using size_type = typename container_type::size_type;

    template <class... Args>
    geometry_collection(Args&&... args) : container_type(std::forward<Args>(args)...)
    {
    }
    geometry_collection(std::initializer_list<geometry_type> args)
        : container_type(std::move(args)) {}
};

} // namespace geometry
} // namespace mapbox
