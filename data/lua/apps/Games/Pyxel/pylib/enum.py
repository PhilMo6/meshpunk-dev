# enum subset for MicroPython: members are plain ints on the class.
# auto() values are assigned per class in definition order, starting at 1.
# Not provided: member objects (.name/.value), iteration over the class, Flag.

_seq = 0


class auto:
    def __init__(self):
        global _seq
        _seq += 1
        self._order = _seq

    def __set_name__(self, owner, name):
        # MicroPython calls __set_name__ in hash order, so the first call for a
        # class resolves every auto() member of that class at once.
        if isinstance(owner.__dict__.get(name), int):
            return
        pending = []
        for key in owner.__dict__:
            val = owner.__dict__[key]
            if isinstance(val, auto):
                pending.append((val._order, key))
        pending.sort()
        for i in range(len(pending)):
            setattr(owner, pending[i][1], i + 1)


class Enum:
    pass


class IntEnum(int):
    pass
