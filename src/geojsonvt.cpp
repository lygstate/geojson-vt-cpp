#include <mapbox/geojsonvt/geojsonvt.hpp>
#include <mapbox/geojsonvt/geojsonvt_clip.hpp>
#include <mapbox/geojsonvt/geojsonvt_convert.hpp>
#include <mapbox/geojsonvt/geojsonvt_util.hpp>

#include <queue>
#include <cmath>

namespace mapbox {
namespace util {
namespace geojsonvt {

std::unordered_map<std::string, clock_t> Time::activities;

#pragma mark - GeoJSONVT

std::vector<ProjectedFeature> GeoJSONVT::convertFeatures(const std::string& data,
                                                         uint8_t maxZoom,
                                                         double tolerance,
                                                         bool debug) {

    if (debug) {
        Time::time("preprocess data");
    }

    uint32_t z2 = 1 << maxZoom;

    JSDocument deserializedData;
    deserializedData.Parse<0>(data.c_str());

    if (deserializedData.HasParseError()) {
        printf("invalid GeoJSON\n");
        return std::vector<ProjectedFeature>();
    }

    const uint16_t extent = 4096;

    std::vector<ProjectedFeature> features =
        Convert::convert(deserializedData, tolerance / (z2 * extent));

    if (debug) {
        Time::timeEnd("preprocess data");
    }

    return features;
}

GeoJSONVT::GeoJSONVT(const std::vector<ProjectedFeature>& features_,
                     uint8_t maxZoom_,
                     uint8_t indexMaxZoom_,
                     uint32_t indexMaxPoints_,
                     double tolerance_,
                     bool debug_)
    : maxZoom(maxZoom_),
      indexMaxZoom(indexMaxZoom_),
      indexMaxPoints(indexMaxPoints_),
      tolerance(tolerance_),
      debug(debug_) {

    if (this->debug) {
        printf("index: maxZoom: %d, maxPoints: %d", indexMaxZoom, indexMaxPoints);
        Time::time("generate tiles");
    }

    splitTile(features_, 0, 0, 0);

    if (this->debug) {
        printf("features: %i, points: %i\n", this->tiles[0].numFeatures, this->tiles[0].numPoints);
        Time::timeEnd("generate tiles");
        printf("tiles generated: %i {\n", this->total);
        for (const auto& pair : this->stats) {
            printf("    z%i: %i\n", pair.first, pair.second);
        }
        printf("}\n");
    }
}

void GeoJSONVT::splitTile(std::vector<ProjectedFeature> features_,
                          uint8_t z_,
                          uint32_t x_,
                          uint32_t y_,
                          int8_t cz,
                          int32_t cx,
                          int32_t cy) {

    std::queue<FeatureStackItem> stack;
    stack.emplace(features_, z_, x_, y_);

    while (stack.size()) {
        FeatureStackItem set = stack.front();
        stack.pop();
        std::vector<ProjectedFeature> features = std::move(set.features);
        uint8_t z = set.z;
        uint32_t x = set.x;
        uint32_t y = set.y;

        uint32_t z2 = 1 << z;
        const uint64_t id = toID(z, x, y);
        Tile* tile;
        double tileTolerance = (z == this->maxZoom ? 0 : this->tolerance / (z2 * this->extent));

        if (this->tiles.count(id)) {
            tile = &this->tiles[id];
        } else {
            if (this->debug) {
                Time::time("creation");
            }

            this->tiles[id] = std::move(
                Tile::createTile(features, z2, x, y, tileTolerance, (z == this->maxZoom)));
            tile = &this->tiles[id];

            if (this->debug) {
                printf("tile z%i-%i-%i (features: %i, points: %i, simplified: %i\n", z, x, y,
                       tile->numFeatures, tile->numPoints, tile->numSimplified);
                Time::timeEnd("creation");

                uint8_t key = z;
                this->stats[key] = (this->stats.count(key) ? this->stats[key] + 1 : 1);
                this->total++;
            }
        }

        // save reference to original geometry in tile so that we can drill down later if we stop now
        tile->source = std::vector<ProjectedFeature>(features);

        // stop tiling if the tile is degenerate
        if (isClippedSquare(tile->features, extent, buffer)) continue;

        // if it's the first-pass tiling
        if (!cz) {
            // stop tiling if we reached max zoom, or if the tile is too simple
            if (z == indexMaxZoom || tile->numPoints <= indexMaxPoints) continue;

        // if a drilldown to a specific tile
        } else {
            // stop tiling if we reached base zoom or our target tile zoom
            if (z == maxZoom || z == cz) continue;

            // stop tiling if it's not an ancestor of the target tile
            const auto m = 1 << (cz - z);
            if (x != std::floor(cx / m) && y != std::floor(cy / m)) continue;
        }

        // if we slice further down, no need to keep source geometry
        tile->source = {};

        if (this->debug) {
            Time::time("clipping");
        }

        double k1 = 0.5 * this->buffer / this->extent;
        double k2 = 0.5 - k1;
        double k3 = 0.5 + k1;
        double k4 = 1 + k1;

        std::vector<ProjectedFeature> tl;
        std::vector<ProjectedFeature> bl;
        std::vector<ProjectedFeature> tr;
        std::vector<ProjectedFeature> br;
        std::vector<ProjectedFeature> left;
        std::vector<ProjectedFeature> right;

        left  = Clip::clip(features, z2, x - k1, x + k3, 0, intersectX, tile->min.x, tile->max.x);
        right = Clip::clip(features, z2, x + k2, x + k4, 0, intersectX, tile->min.x, tile->max.x);

        if (left.size()) {
            tl = Clip::clip(left, z2, y - k1, y + k3, 1, intersectY, tile->min.y, tile->max.y);
            bl = Clip::clip(left, z2, y + k2, y + k4, 1, intersectY, tile->min.y, tile->max.y);
        }

        if (right.size()) {
            tr = Clip::clip(right, z2, y - k1, y + k3, 1, intersectY, tile->min.y, tile->max.y);
            br = Clip::clip(right, z2, y + k2, y + k4, 1, intersectY, tile->min.y, tile->max.y);
        }

        if (this->debug) {
            Time::timeEnd("clipping");
        }

        if (tl.size())
            stack.emplace(std::move(tl), z + 1, x * 2, y * 2);
        if (bl.size())
            stack.emplace(std::move(bl), z + 1, x * 2, y * 2 + 1);
        if (tr.size())
            stack.emplace(std::move(tr), z + 1, x * 2 + 1, y * 2);
        if (br.size())
            stack.emplace(std::move(br), z + 1, x * 2 + 1, y * 2 + 1);
    }
}

Tile& GeoJSONVT::getTile(uint8_t z, uint32_t x, uint32_t y) {

    std::lock_guard<std::mutex> lock(mtx);

    const uint64_t id = toID(z, x, y);
    if (this->tiles.count(id)) {
        return transformTile(this->tiles.find(id)->second, extent);
    }

    if (this->debug) {
        printf("drilling down to z%i-%i-%i\n", z, x, y);
    }

    uint8_t z0 = z;
    uint32_t x0 = x;
    uint32_t y0 = y;
    Tile* parent = nullptr;

    while (!parent && z0) {
        z0--;
        x0 = x0 / 2;
        y0 = y0 / 2;
        const uint64_t checkID = toID(z0, x0, y0);
        if (this->tiles.count(checkID)) {
            parent = &this->tiles[checkID];
        }
    }

    if (this->debug) {
        printf("found parent tile z%i-%i-%i\n", z0, x0, y0);
    }

    // if we found a parent tile containing the original geometry, we can drill down from it
    if (parent->source.size()) {
        if (isClippedSquare(parent->features, this->extent, this->buffer)) {
            return transformTile(*parent, extent);
        }

        if (this->debug) {
            Time::time("drilling down");
        }

        splitTile(parent->source, z0, x0, y0, z, x, y);

        if (this->debug) {
            Time::timeEnd("drilling down");
        }
    }

    return transformTile(this->tiles[id], extent);
}

Tile& GeoJSONVT::transformTile(Tile& tile, uint16_t extent) {
    if (tile.transformed) {
        return tile;
    }

    const uint32_t z2 = tile.z2;
    const uint32_t tx = tile.tx;
    const uint32_t ty = tile.ty;

    for (size_t i = 0; i < tile.features.size(); i++) {
        auto& feature = tile.features[i];
        const auto& geom = feature.geometry;
        const auto type = feature.type;

        if (type == TileFeatureType::Point) {
            for (const auto& pt : geom) {
                feature.tileGeometry.push_back(transformPoint(pt.get<ProjectedPoint>(), extent, z2, tx, ty));
            }

        } else {
            for (const auto& r : geom) {
                TileRing ring;
                for (const auto& pt : r.get<ProjectedGeometryContainer>().members) {
                    ring.points.push_back(transformPoint(pt.get<ProjectedPoint>(), extent, z2, tx, ty));
                }
                feature.tileGeometry.emplace_back(std::move(ring));
            }
        }
    }

    tile.transformed = true;

    return tile;
}

TilePoint GeoJSONVT::transformPoint(
    const ProjectedPoint& p, uint16_t extent, uint32_t z2, uint32_t tx, uint32_t ty) {

    int16_t x = std::round(extent * (p.x * z2 - tx));
    int16_t y = std::round(extent * (p.y * z2 - ty));

    return TilePoint(x, y);
}

bool GeoJSONVT::isClippedSquare(const std::vector<TileFeature>& features,
                                uint16_t extent_,
                                uint8_t buffer_) const {

    if (features.size() != 1) {
        return false;
    }

    const TileFeature feature = features.front();

    if (feature.type != TileFeatureType::Polygon || feature.geometry.size() > 1) {
        return false;
    }

    const ProjectedGeometryContainer* container = &(feature.geometry.front().get<ProjectedGeometryContainer>());

    for (size_t i = 0; i < container->members.size(); ++i) {
        const ProjectedPoint* p = &container->members[i].get<ProjectedPoint>();
        if ((p->x != -buffer_ && p->x != extent_ + buffer_) ||
            (p->y != -buffer_ && p->y != extent_ + buffer_)) {
            return false;
        }
    }

    return true;
}

uint64_t GeoJSONVT::toID(uint8_t z, uint32_t x, uint32_t y) {

    return (((1 << z) * y + x) * 32) + z;
}

ProjectedPoint GeoJSONVT::intersectX(const ProjectedPoint& a, const ProjectedPoint& b, double x) {

    double r1 = x;
    double r2 = (x - a.x) * (b.y - a.y) / (b.x - a.x) + a.y;
    double r3 = 1;

    return ProjectedPoint(r1, r2, r3);
}

ProjectedPoint GeoJSONVT::intersectY(const ProjectedPoint& a, const ProjectedPoint& b, double y) {

    double r1 = (y - a.y) * (b.x - a.x) / (b.y - a.y) + a.x;
    double r2 = y;
    double r3 = 1;

    return ProjectedPoint(r1, r2, r3);
}

} // namespace geojsonvt
} // namespace util
} // namespace mapbox
