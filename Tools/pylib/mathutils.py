# Shim pur Python du module mathutils de Blender, limite au strict necessaire de
# bpypolyskel (Vector 2D/3D + geometry.intersect_point_line). Permet d'executer la
# lib avec un Python standard (celui embarque par Blender, sans lancer Blender) et
# donc d'avoir multiprocessing/timeouts propres. Semantique copiee de mathutils :
# cross 2D -> scalaire, cross 3D -> Vector, intersect_point_line -> (point, facteur).
import math


class Vector:
    __slots__ = ("_v",)

    def __init__(self, seq=(0.0, 0.0, 0.0)):
        self._v = [float(c) for c in seq]

    # --- acces ---
    @property
    def x(self):
        return self._v[0]

    @x.setter
    def x(self, val):
        self._v[0] = float(val)

    @property
    def y(self):
        return self._v[1]

    @y.setter
    def y(self, val):
        self._v[1] = float(val)

    @property
    def z(self):
        return self._v[2]

    @z.setter
    def z(self, val):
        self._v[2] = float(val)

    @property
    def xy(self):
        return Vector(self._v[:2])

    def __getitem__(self, i):
        return self._v[i]

    def __setitem__(self, i, val):
        self._v[i] = float(val)

    def __len__(self):
        return len(self._v)

    def __iter__(self):
        return iter(self._v)

    def __repr__(self):
        return "Vector(({}))".format(", ".join(repr(c) for c in self._v))

    # --- comparaison (elementwise, comme mathutils) ---
    def __eq__(self, other):
        if not isinstance(other, Vector) or len(other._v) != len(self._v):
            return False
        return self._v == other._v

    def __ne__(self, other):
        return not self.__eq__(other)

    __hash__ = None  # mutable, non hashable (comme mathutils non gele)

    # --- arithmetique ---
    def __add__(self, other):
        return Vector(a + b for a, b in zip(self._v, other._v))

    def __sub__(self, other):
        return Vector(a - b for a, b in zip(self._v, other._v))

    def __neg__(self):
        return Vector(-a for a in self._v)

    def __mul__(self, s):
        if isinstance(s, Vector):
            raise TypeError("Vector*Vector non supporte : utiliser dot()")
        return Vector(a * s for a in self._v)

    __rmul__ = __mul__

    def __truediv__(self, s):
        return Vector(a / s for a in self._v)

    # --- operations ---
    def copy(self):
        return Vector(self._v)

    def dot(self, other):
        return sum(a * b for a, b in zip(self._v, other._v))

    def cross(self, other):
        if len(self._v) == 2 and len(other._v) == 2:
            return self._v[0] * other._v[1] - self._v[1] * other._v[0]
        ax, ay, az = self._v[0], self._v[1], self._v[2]
        bx, by, bz = other._v[0], other._v[1], other._v[2]
        return Vector((ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx))

    @property
    def length(self):
        return math.sqrt(sum(a * a for a in self._v))

    magnitude = length

    @property
    def length_squared(self):
        return sum(a * a for a in self._v)

    def normalized(self):
        l = self.length
        if l == 0.0:
            return Vector([0.0] * len(self._v))
        return Vector(a / l for a in self._v)

    def normalize(self):
        # Version in-place (contrat mathutils : retourne None).
        l = self.length
        if l != 0.0:
            self._v = [a / l for a in self._v]

    def negate(self):
        self._v = [-a for a in self._v]

    def freeze(self):
        return self


class geometry:
    @staticmethod
    def intersect_point_line(pt, line_p1, line_p2):
        # Point le plus proche sur la DROITE (non clampe) + facteur parametrique,
        # meme contrat que mathutils.geometry.intersect_point_line.
        n = min(len(pt), len(line_p1), len(line_p2))
        d = [line_p2[i] - line_p1[i] for i in range(n)]
        dd = sum(c * c for c in d)
        if dd == 0.0:
            return Vector(line_p1[:n]), 0.0
        t = sum((pt[i] - line_p1[i]) * d[i] for i in range(n)) / dd
        return Vector(line_p1[i] + d[i] * t for i in range(n)), t
