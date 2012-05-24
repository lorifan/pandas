#include <Python.h>
#define PY_ARRAY_UNIQUE_SYMBOL UJSON_NUMPY
#define NO_IMPORT_ARRAY
#include <numpy/arrayobject.h>
#include <ultrajson.h>


typedef struct __PyObjectDecoder
{
    JSONObjectDecoder dec;

    void* npyarr;       // Numpy context buffer
    npy_intp curdim;    // Current array dimension 

    PyArray_Descr* dtype;
} PyObjectDecoder;

typedef struct __NpyArrContext
{
    PyObject* ret;
    PyObject* labels[2];
    PyArray_Dims shape;

    PyObjectDecoder* dec;

    npy_intp i;
    npy_intp elsize;
    npy_intp elcount;
} NpyArrContext;

//#define PRINTMARK() fprintf(stderr, "%s: MARK(%d)\n", __FILE__, __LINE__)     
#define PRINTMARK()

// Numpy handling based on numpy internal code, specifically the function
// PyArray_FromIter.

// numpy related functions are inter-dependent so declare them all here,
// to ensure the compiler catches any errors

// standard numpy array handling
JSOBJ Object_npyNewArray(void* decoder);
JSOBJ Object_npyEndArray(JSOBJ obj);
int Object_npyArrayAddItem(JSOBJ obj, JSOBJ value);

// for more complex dtypes (object and string) fill a standard Python list
// and convert to a numpy array when done.
JSOBJ Object_npyNewArrayList(void* decoder);
JSOBJ Object_npyEndArrayList(JSOBJ obj);
int Object_npyArrayListAddItem(JSOBJ obj, JSOBJ value);

// labelled support, encode keys and values of JS object into separate numpy
// arrays
JSOBJ Object_npyNewObject(void* decoder);
JSOBJ Object_npyEndObject(JSOBJ obj);
int Object_npyObjectAddKey(JSOBJ obj, JSOBJ name, JSOBJ value);


// free the numpy context buffer
void Npy_releaseContext(NpyArrContext* npyarr) 
{
    PRINTMARK();
    if (npyarr) 
    {
        if (npyarr->shape.ptr)
        {
            PyObject_Free(npyarr->shape.ptr);
        }
        if (npyarr->dec)
        {
            // Don't set to null, used to make sure we don't Py_DECREF npyarr
            // in releaseObject
            // npyarr->dec->npyarr = NULL;
            npyarr->dec->curdim = 0;
        }
        Py_XDECREF(npyarr->labels[0]);
        Py_XDECREF(npyarr->labels[1]);
        Py_XDECREF(npyarr->ret);
        PyObject_Free(npyarr);
    }
}

JSOBJ Object_npyNewArray(void* _decoder)
{
    PRINTMARK();
    PyObjectDecoder* decoder = (PyObjectDecoder*) _decoder;
    NpyArrContext* npyarr;
    if (decoder->curdim <= 0)
    {
        // start of array - initialise the context buffer
        npyarr = decoder->npyarr = PyObject_Malloc(sizeof(NpyArrContext));

        if (!npyarr)
        {
            PyErr_NoMemory();
            return NULL;
        }

        npyarr->dec = decoder;
        npyarr->labels[0] = npyarr->labels[1] = NULL;

        npyarr->shape.ptr = PyObject_Malloc(sizeof(npy_intp)*NPY_MAXDIMS);
        npyarr->shape.len = 1;
        npyarr->ret = NULL;

        npyarr->elsize = 0;
        npyarr->elcount = 4;
        npyarr->i = 0;
    }
    else
    {
        // starting a new dimension continue the current array (and reshape after) 
        npyarr = (NpyArrContext*) decoder->npyarr;
        if (decoder->curdim >= npyarr->shape.len)
        {
            npyarr->shape.len++;
        }
    }

    npyarr->shape.ptr[decoder->curdim] = 0;
    decoder->curdim++;
    return npyarr;
}

JSOBJ Object_npyEndArray(JSOBJ obj)
{
    PRINTMARK();
    NpyArrContext* npyarr = (NpyArrContext*) obj;
    if (!npyarr)
    {
        return NULL;
    }

    PyObject* ret = npyarr->ret;
    int emptyType = NPY_DEFAULT_TYPE;
    npy_intp i = npyarr->i;
    char* new_data;

    npyarr->dec->curdim--;

    if (i == 0 || !npyarr->ret) {
        // empty array would not have been initialised so do it now.
        if (npyarr->dec->dtype)
        {
            emptyType = npyarr->dec->dtype->type_num;
        }
        npyarr->ret = ret = PyArray_EMPTY(npyarr->shape.len, npyarr->shape.ptr, emptyType, 0);
    }
    else if (npyarr->dec->curdim <= 0)
    {
        // realloc to final size 
        new_data = PyDataMem_RENEW(PyArray_DATA(ret), i * npyarr->elsize);
        if (new_data == NULL) {
            PyErr_NoMemory();
            Npy_releaseContext(npyarr);
            return NULL;
        }
        ((char*)PyArray_DATA(ret)) = new_data;
    }

    if (npyarr->dec->curdim <= 0)
    {
        // finished decoding array, reshape if necessary
        if (npyarr->shape.len > 1)
        {
            npyarr->ret = PyArray_Newshape((PyArrayObject*) ret, &npyarr->shape, NPY_ANYORDER);
            Py_DECREF(ret);
            ret = npyarr->ret;
        }

        if (npyarr->labels[0] || npyarr->labels[1])
        {
            // finished decoding, build tuple with values and labels
            ret = PyTuple_New(npyarr->shape.len+1);
            for (i = 0; i < npyarr->shape.len; i++)
            {
                if (npyarr->labels[i])
                {
                    PyTuple_SET_ITEM(ret, i+1, npyarr->labels[i]);
                    npyarr->labels[i] = NULL;
                }
                else
                {
                    Py_INCREF(Py_None);
                    PyTuple_SET_ITEM(ret, i+1, Py_None);
                }
            }
            PyTuple_SET_ITEM(ret, 0, npyarr->ret);
        }
        npyarr->ret = NULL;
        Npy_releaseContext(npyarr);
    }
    
    return ret;
}

int Object_npyArrayAddItem(JSOBJ obj, JSOBJ value)
{
    PRINTMARK();
    NpyArrContext* npyarr = (NpyArrContext*) obj;
    if (!npyarr)
    {
        return 0;
    }

    PyObject* type;
    PyArray_Descr* dtype;
    npy_intp i = npyarr->i;
    char *new_data, *item;

    npyarr->shape.ptr[npyarr->dec->curdim-1]++;

    if (PyArray_Check(value))
    {
        // multidimensional array, keep decoding values.
        return 1;
    }

    if (!npyarr->ret)
    {
        // Array not initialised yet.
        // We do it here so we can 'sniff' the data type if none was provided
        if (!npyarr->dec->dtype)
        {
            type = PyObject_Type(value);
            if(!PyArray_DescrConverter(type, &dtype)) 
            {
                Py_DECREF(type);
                goto fail;
            }
            Py_INCREF(dtype);
            Py_DECREF(type);
        }
        else 
        {
            dtype = PyArray_DescrNew(npyarr->dec->dtype);
        }

        // If it's an object or string then fill a Python list and subsequently 
        // convert. Otherwise we would need to somehow mess about with 
        // reference counts when renewing memory.
        npyarr->elsize = dtype->elsize;
        if (PyDataType_REFCHK(dtype) || npyarr->elsize == 0) 
        {
            Py_XDECREF(dtype);

            if (npyarr->dec->curdim > 1) 
            {
                PyErr_SetString(PyExc_ValueError, "Cannot decode multidimensional arrays with variable length elements to numpy");
                goto fail;
            }
            npyarr->ret = PyList_New(0);
            if (!npyarr->ret) 
            {
                goto fail;
            }
            ((JSONObjectDecoder*)npyarr->dec)->newArray = Object_npyNewArrayList;
            ((JSONObjectDecoder*)npyarr->dec)->arrayAddItem = Object_npyArrayListAddItem;
            ((JSONObjectDecoder*)npyarr->dec)->endArray = Object_npyEndArrayList;
            return Object_npyArrayListAddItem(obj, value);
        }

        npyarr->ret = PyArray_NewFromDescr(&PyArray_Type, dtype, 1,
                                           &npyarr->elcount, NULL,NULL, 0, NULL);

        if (!npyarr->ret) 
        {
            goto fail;
        }
    }

    if (i >= npyarr->elcount) {
        // Grow PyArray_DATA(ret):
        // this is similar for the strategy for PyListObject, but we use
        // 50% overallocation => 0, 4, 8, 14, 23, 36, 56, 86 ...
        if (npyarr->elsize == 0)
        {
            PyErr_SetString(PyExc_ValueError, "Cannot decode multidimensional arrays with variable length elements to numpy");
            goto fail;
        }

        npyarr->elcount = (i >> 1) + (i < 4 ? 4 : 2) + i;
        if (npyarr->elcount <= NPY_MAX_INTP/npyarr->elsize) {
            new_data = PyDataMem_RENEW(PyArray_DATA(npyarr->ret), npyarr->elcount * npyarr->elsize);
        }
        else {
            PyErr_NoMemory();
            goto fail;
        }
        ((char*)PyArray_DATA(npyarr->ret)) = new_data;
    }

    PyArray_DIMS(npyarr->ret)[0] = i + 1;

    if ((item = PyArray_GETPTR1(npyarr->ret, i)) == NULL
            || PyArray_SETITEM(npyarr->ret, item, value) == -1) {
        goto fail;
    }

    Py_DECREF( (PyObject *) value);
    npyarr->i++;
    return 1;

fail:

    Npy_releaseContext(npyarr);
    return 0;
}

JSOBJ Object_npyNewArrayList(void* _decoder)
{
    PRINTMARK();
    PyObjectDecoder* decoder = (PyObjectDecoder*) _decoder;
    PyErr_SetString(PyExc_ValueError, "nesting not supported for object or variable length dtypes");
    Npy_releaseContext(decoder->npyarr);
    return NULL;
}

JSOBJ Object_npyEndArrayList(JSOBJ obj)
{
    PRINTMARK();
    NpyArrContext* npyarr = (NpyArrContext*) obj;
    if (!npyarr)
    {
        return NULL;
    }

    // convert decoded list to numpy array
    PyObject* list = (PyObject *) npyarr->ret;
    PyObject* ret = PyArray_FROM_O(list);

    ((JSONObjectDecoder*)npyarr->dec)->newArray = Object_npyNewArray;
    ((JSONObjectDecoder*)npyarr->dec)->arrayAddItem = Object_npyArrayAddItem;
    ((JSONObjectDecoder*)npyarr->dec)->endArray = Object_npyEndArray;
    Npy_releaseContext(npyarr);
    return ret; 
}

int Object_npyArrayListAddItem(JSOBJ obj, JSOBJ value)
{
    PRINTMARK();
    NpyArrContext* npyarr = (NpyArrContext*) obj;
    if (!npyarr)
    {
        return 0;
    }
    PyList_Append((PyObject*) npyarr->ret, value);
    Py_DECREF( (PyObject *) value);
    return 1;
}


JSOBJ Object_npyNewObject(void* _decoder)
{
    PRINTMARK();
    PyObjectDecoder* decoder = (PyObjectDecoder*) _decoder;
    if (decoder->curdim > 1)
    {
        PyErr_SetString(PyExc_ValueError, "labels only supported up to 2 dimensions");
        return NULL;
    }

    return ((JSONObjectDecoder*)decoder)->newArray(decoder);
}

JSOBJ Object_npyEndObject(JSOBJ obj)
{
    PRINTMARK();
    NpyArrContext* npyarr = (NpyArrContext*) obj;
    if (!npyarr)
    {
        return NULL;
    }

    npy_intp labelidx = npyarr->dec->curdim-1;

    PyObject* list = npyarr->labels[labelidx];
    if (list)
    {
        npyarr->labels[labelidx] = PyArray_FROM_O(list);
        Py_DECREF(list);
    }

    return (PyObject*) ((JSONObjectDecoder*)npyarr->dec)->endArray(obj);
}

int Object_npyObjectAddKey(JSOBJ obj, JSOBJ name, JSOBJ value)
{
    PRINTMARK();
    // add key to label array, value to values array
    NpyArrContext* npyarr = (NpyArrContext*) obj;
    if (!npyarr)
    {
        return 0;
    }

    PyObject* label = (PyObject*) name;
    npy_intp labelidx = npyarr->dec->curdim-1;

    if (!npyarr->labels[labelidx])
    {
        npyarr->labels[labelidx] = PyList_New(0);
    }

    // only fill label array once, assumes all column labels are the same
    // for 2-dimensional arrays.
    if (PyList_GET_SIZE(npyarr->labels[labelidx]) <= npyarr->elcount)
    {
        PyList_Append(npyarr->labels[labelidx], label);
    }

    if(((JSONObjectDecoder*)npyarr->dec)->arrayAddItem(obj, value))
    {
        Py_DECREF(label);
        return 1;
    }
    return 0;
}

int Object_objectAddKey(JSOBJ obj, JSOBJ name, JSOBJ value)
{
    PyDict_SetItem (obj, name, value);
    Py_DECREF( (PyObject *) name);
    Py_DECREF( (PyObject *) value);
    return 1;
}

int Object_arrayAddItem(JSOBJ obj, JSOBJ value)
{
    PyList_Append(obj, value);
    Py_DECREF( (PyObject *) value);
    return 1;
}

JSOBJ Object_newString(wchar_t *start, wchar_t *end)
{
    return PyUnicode_FromWideChar (start, (end - start));
}

JSOBJ Object_newTrue(void)
{ 
    Py_RETURN_TRUE;
}

JSOBJ Object_newFalse(void)
{
    Py_RETURN_FALSE;
}

JSOBJ Object_newNull(void)
{
    Py_RETURN_NONE;
}

JSOBJ Object_newObject(void* decoder)
{
    return PyDict_New();
}

JSOBJ Object_endObject(JSOBJ obj)
{
    return obj;
}

JSOBJ Object_newArray(void* decoder)
{
    return PyList_New(0);
}

JSOBJ Object_endArray(JSOBJ obj)
{
    return obj;
}

JSOBJ Object_newInteger(JSINT32 value)
{
    return PyInt_FromLong( (long) value);
}

JSOBJ Object_newLong(JSINT64 value)
{
    return PyLong_FromLongLong (value);
}

JSOBJ Object_newDouble(double value)
{ 
    return PyFloat_FromDouble(value);
}

static void Object_releaseObject(JSOBJ obj, void* _decoder)
{
    PyObjectDecoder* decoder = (PyObjectDecoder*) _decoder;
    if (obj != decoder->npyarr)
    {
        Py_XDECREF( ((PyObject *)obj));
    }
}


PyObject* JSONToObj(PyObject* self, PyObject *args, PyObject *kwargs)
{
    PRINTMARK();
    static char *kwlist[] = { "obj", "numpy", "labelled", "dtype", NULL};

    PyObject *ret;
    PyObject *sarg;
    PyArray_Descr *dtype = NULL;
    int numpy = 0, labelled = 0, decref = 0;

    PyObjectDecoder pyDecoder =
    {
        {
            Object_newString,
            Object_objectAddKey,
            Object_arrayAddItem,
            Object_newTrue,
            Object_newFalse,
            Object_newNull,
            Object_newObject,
            Object_endObject,
            Object_newArray,
            Object_endArray,
            Object_newInteger,
            Object_newLong,
            Object_newDouble,
            Object_releaseObject,
            PyObject_Malloc,
            PyObject_Free,
            PyObject_Realloc,
        }
    };

    pyDecoder.curdim = 0;
    pyDecoder.npyarr = NULL;

    JSONObjectDecoder* decoder = (JSONObjectDecoder*) &pyDecoder;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|iiO&", kwlist, &sarg, &numpy, &labelled, PyArray_DescrConverter, &dtype))
    {
        return NULL;
    }

    if (PyUnicode_Check(sarg))
    {
        sarg = PyUnicode_AsUTF8String(sarg);
        if (sarg == NULL)
        {
            //Exception raised above us by codec according to docs
            return NULL;
        }
        decref = 1;
    }
    else
    if (!PyString_Check(sarg))
    {
        PyErr_Format(PyExc_TypeError, "Expected String or Unicode");
        return NULL;
    }

    if (numpy)
    {
        pyDecoder.dtype = dtype;
        decoder->newArray = Object_npyNewArray;
        decoder->endArray = Object_npyEndArray;
        decoder->arrayAddItem = Object_npyArrayAddItem;

        if (labelled)
        {
            decoder->newObject = Object_npyNewObject;
            decoder->endObject = Object_npyEndObject;
            decoder->objectAddKey = Object_npyObjectAddKey;
        }
    }

    decoder->errorStr = NULL;
    decoder->errorOffset = NULL;

    PRINTMARK();
    ret = JSON_DecodeObject(decoder, PyString_AS_STRING(sarg), PyString_GET_SIZE(sarg));
    PRINTMARK();

    if (decref)
    {
        Py_DECREF(sarg);
    }

    if (PyErr_Occurred())
    {
        return NULL;
    }

    if (decoder->errorStr)
    {
        /*FIXME: It's possible to give a much nicer error message here with actual failing element in input etc*/
        PyErr_Format (PyExc_ValueError, "%s", decoder->errorStr);
        Py_XDECREF( (PyObject *) ret);
        Npy_releaseContext(pyDecoder.npyarr);

        return NULL;
    }

    return ret;
}

PyObject* JSONFileToObj(PyObject* self, PyObject *args, PyObject *kwargs)
{
    PyObject *file;
    PyObject *read;
    PyObject *string;
    PyObject *result;
    PyObject *argtuple;

    if (!PyArg_ParseTuple (args, "O", &file)) {
        return NULL;
    }

    if (!PyObject_HasAttrString (file, "read"))
    {
        PyErr_Format (PyExc_TypeError, "expected file");
        return NULL;
    }

    read = PyObject_GetAttrString (file, "read");

    if (!PyCallable_Check (read)) {
        Py_XDECREF(read);
        PyErr_Format (PyExc_TypeError, "expected file");
        return NULL;
    }

    string = PyObject_CallObject (read, NULL);
    Py_XDECREF(read);

    if (string == NULL)
    {
        return NULL;
    }

    argtuple = PyTuple_Pack(1, string);

    result = JSONToObj (self, argtuple, kwargs);
    Py_XDECREF(string);
    Py_DECREF(argtuple);

    if (result == NULL) {
        return NULL;
    }

    return result;
}
