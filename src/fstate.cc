#include <map>
#include <iostream>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include "fstate.hh"
#include "globals.hh"
#include "store.hh"
#include "db.hh"
#include "references.hh"


/* A Unix environment is a mapping from strings to strings. */
typedef map<string, string> Environment;


class AutoDelete
{
    string path;
public:

    AutoDelete(const string & p) : path(p) 
    {
    }

    ~AutoDelete()
    {
        deletePath(path);
    }
};


/* Run a program. */
static void runProgram(const string & program, Environment env)
{
    /* Create a log file. */
    string logFileName = nixLogDir + "/run.log";
    /* !!! auto-pclose on exit */
    FILE * logFile = popen(("tee -a " + logFileName + " >&2").c_str(), "w"); /* !!! escaping */
    if (!logFile)
        throw SysError(format("creating log file `%1%'") % logFileName);

    /* Create a temporary directory where the build will take
       place. */
    static int counter = 0;
    string tmpDir = (format("/tmp/nix-%1%-%2%") % getpid() % counter++).str();

    if (mkdir(tmpDir.c_str(), 0777) == -1)
        throw SysError(format("creating directory `%1%'") % tmpDir);

    AutoDelete delTmpDir(tmpDir);

    /* Fork a child to build the package. */
    pid_t pid;
    switch (pid = fork()) {
            
    case -1:
        throw SysError("unable to fork");

    case 0: 

        try { /* child */

            if (chdir(tmpDir.c_str()) == -1)
                throw SysError(format("changing into to `%1%'") % tmpDir);

            /* Fill in the environment.  We don't bother freeing
               the strings, since we'll exec or die soon
               anyway. */
            const char * env2[env.size() + 1];
            int i = 0;
            for (Environment::iterator it = env.begin();
                 it != env.end(); it++, i++)
                env2[i] = (new string(it->first + "=" + it->second))->c_str();
            env2[i] = 0;

            /* Dup the log handle into stderr. */
            if (dup2(fileno(logFile), STDERR_FILENO) == -1)
                throw SysError("cannot pipe standard error into log file");
            
            /* Dup stderr to stdin. */
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1)
                throw SysError("cannot dup stderr into stdout");

            /* Make the program executable.  !!! hack. */
            if (chmod(program.c_str(), 0755))
                throw SysError("cannot make program executable");

            /* Execute the program.  This should not return. */
            execle(program.c_str(), baseNameOf(program).c_str(), 0, env2);

            throw SysError(format("unable to execute %1%") % program);
            
        } catch (exception & e) {
            cerr << format("build error: %1%\n") % e.what();
        }
        _exit(1);

    }

    /* parent */

    /* Close the logging pipe.  Note that this should not cause
       the logger to exit until builder exits (because the latter
       has an open file handle to the former). */
    pclose(logFile);
    
    /* Wait for the child to finish. */
    int status;
    if (waitpid(pid, &status, 0) != pid)
        throw Error("unable to wait for child");
    
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw Error("unable to build package");
}


/* Throw an exception if the given platform string is not supported by
   the platform we are executing on. */
static void checkPlatform(const string & platform)
{
    if (platform != thisSystem)
        throw Error(format("a `%1%' is required, but I am a `%2%'")
            % platform % thisSystem);
}


string printTerm(ATerm t)
{
    char * s = ATwriteToString(t);
    return s;
}


Error badTerm(const format & f, ATerm t)
{
    return Error(format("%1%, in `%2%'") % f.str() % printTerm(t));
}


Hash hashTerm(ATerm t)
{
    return hashString(printTerm(t));
}


FState hash2fstate(Hash hash)
{
    return ATmake("Include(<str>)", ((string) hash).c_str());
}


ATerm termFromId(const FSId & id, string * p)
{
    string path = expandId(id);
    if (p) *p = path;
    ATerm t = ATreadFromNamedFile(path.c_str());
    if (!t) throw Error(format("cannot read aterm from `%1%'") % path);
    return t;
}


FSId writeTerm(ATerm t, const string & suffix, string * p)
{
    FSId id = hashTerm(t);

    string path = canonPath(nixStore + "/" + 
        (string) id + suffix + ".nix");
    if (!ATwriteToNamedTextFile(t, path.c_str()))
        throw Error(format("cannot write aterm %1%") % path);

    registerPath(path, id);
    if (p) *p = path;

    return id;
}


void registerSuccessor(const FSId & id1, const FSId & id2)
{
    setDB(nixDB, dbSuccessors, id1, id2);
}


static FSId storeSuccessor(const FSId & id1, FState sc)
{
    FSId id2 = writeTerm(sc, "-s-" + (string) id1, 0);
    registerSuccessor(id1, id2);
    return id2;
}


#if 0
static FState realise(FState fs, StringSet & paths)
{
    char * s1, * s2, * s3;
    Content content;
    ATermList refs, ins, bnds;

    /* First repeatedly try to substitute $fs$ by any known successors
       in order to speed up the rewrite process. */
    {
        string fsHash, scHash;
        while (queryDB(nixDB, dbSuccessors, fsHash = hashTerm(fs), scHash)) {
            debug(format("successor %1% -> %2%") % (string) fsHash % scHash);
            string path;
            FState fs2 = termFromHash(parseHash(scHash), &path);
            paths.insert(path);
            if (fs == fs2) {
                debug(format("successor cycle detected in %1%") % printTerm(fs));
                break;
            }
            fs = fs2;
        }
    }

    /* Fall through. */

    if (ATmatch(fs, "Include(<str>)", &s1)) {
        string path;
        fs = termFromHash(parseHash(s1), &path);
        paths.insert(path);
        return realise(fs, paths);
    }
    
    else if (ATmatch(fs, "Path(<str>, <term>, [<list>])", &s1, &content, &refs)) {
        string path(s1);

        msg(format("realising atomic path %1%") % path);
        Nest nest(true);

        if (path[0] != '/')
            throw Error(format("path `%1% is not absolute") % path);

        /* Realise referenced paths. */
        ATermList refs2 = ATempty;
        while (!ATisEmpty(refs)) {
            refs2 = ATinsert(refs2, realise(ATgetFirst(refs), paths));
            refs = ATgetNext(refs);
        }
        refs2 = ATreverse(refs2);

        if (!ATmatch(content, "Hash(<str>)", &s1))
            throw badTerm("hash expected", content);
        Hash hash = parseHash(s1);

        /* Normal form. */
        ATerm nf = ATmake("Path(<str>, <term>, <term>)",
            path.c_str(), content, refs2);

        /* Register the normal form. */
        nf = storeSuccessor(fs, nf, paths);
        
        /* Expand the hash into the target path. */
        expandHash(hash, path);

        return nf;
    }

    else if (ATmatch(fs, "Derive(<str>, <str>, [<list>], <str>, [<list>])",
                 &s1, &s2, &ins, &s3, &bnds)) 
    {
        string platform(s1), builder(s2), outPath(s3);

        msg(format("realising derivate path %1%") % outPath);
        Nest nest(true);

        checkPlatform(platform);
        
        /* Realise inputs. */
        Strings inPaths;
        ATermList ins2 = ATempty;
        while (!ATisEmpty(ins)) {
            FState in = realise(ATgetFirst(ins), paths);
            inPaths.push_back(fstatePath(in));
            ins2 = ATinsert(ins2, in);
            ins = ATgetNext(ins);
        }
        ins = ATreverse(ins2);

        /* Build the environment. */
        Environment env;
        while (!ATisEmpty(bnds)) {
            ATerm bnd = ATgetFirst(bnds);
            if (!ATmatch(bnd, "(<str>, <str>)", &s1, &s2))
                throw badTerm("tuple of strings expected", bnd);
            env[s1] = s2;
            bnds = ATgetNext(bnds);
        }

        /* Check whether the target already exists. */
        if (pathExists(outPath))
            deleteFromStore(outPath);
//             throw Error(format("path %1% already exists") % outPath);

        /* Run the builder. */
        runProgram(builder, env);
        
        /* Check whether the result was created. */
        if (!pathExists(outPath))
            throw Error(format("program %1% failed to create a result in %2%")
                % builder % outPath);

#if 0
        /* Remove write permission from the value. */
        int res = system(("chmod -R -w " + targetPath).c_str()); // !!! escaping
        if (WEXITSTATUS(res) != 0)
            throw Error("cannot remove write permission from " + targetPath);
#endif

        /* Hash the result. */
        Hash outHash = hashPath(outPath);

        /* Register targetHash -> targetPath.  !!! this should be in
           values.cc. */
        registerPath(outPath, outHash);

        /* Filter out inputs that are not referenced in the output. */
        for (Strings::iterator i = inPaths.begin();
             i != inPaths.end(); i++)
            debug(format("in: %1%") % *i);

        Strings outPaths = filterReferences(outPath, inPaths);

        for (Strings::iterator i = outPaths.begin();
             i != outPaths.end(); i++)
            debug(format("out: %1%") % *i);

        ins2 = ATempty;
        while (!ATisEmpty(ins)) {
            FState in = ATgetFirst(ins);
            string path = fstatePath(in);
            for (Strings::iterator i = outPaths.begin();
                 i != outPaths.end(); i++)
                if (path.find(*i) != string::npos) {
                    debug(format("out2: %1%") % path);
                    ins2 = ATinsert(ins2, in);
                }
            ins = ATgetNext(ins);
        }
        ins = ATreverse(ins2);
        
        /* Register the normal form of fs. */
        FState nf = ATmake("Path(<str>, Hash(<str>), <term>)",
            outPath.c_str(), ((string) outHash).c_str(), ins);
        nf = storeSuccessor(fs, nf, paths);

        return nf;
    }

    throw badTerm("bad fstate expression", fs);
}


FState realiseFState(FState fs, StringSet & paths)
{
    return realise(fs, paths);
}


string fstatePath(FState fs)
{
    char * s1, * s2, * s3;
    FState e1, e2;
    if (ATmatch(fs, "Path(<str>, <term>, [<list>])", &s1, &e1, &e2))
        return s1;
    else if (ATmatch(fs, "Derive(<str>, <str>, [<list>], <str>, [<list>])",
                   &s1, &s2, &e1, &s3, &e2))
        return s3;
    else if (ATmatch(fs, "Include(<str>)", &s1))
        return fstatePath(termFromHash(parseHash(s1)));
    else
        return "";
}


void fstateRefs2(FState fs, StringSet & paths)
{
    char * s1, * s2, * s3;
    FState e1, e2;
    ATermList refs, ins;

    if (ATmatch(fs, "Path(<str>, <term>, [<list>])", &s1, &e1, &refs)) {
        paths.insert(s1);

        while (!ATisEmpty(refs)) {
            fstateRefs2(ATgetFirst(refs), paths);
            refs = ATgetNext(refs);
        }
    }

    else if (ATmatch(fs, "Derive(<str>, <str>, [<list>], <str>, [<list>])", 
            &s1, &s2, &ins, &s3, &e2))
    {
        while (!ATisEmpty(ins)) {
            fstateRefs2(ATgetFirst(ins), paths);
            ins = ATgetNext(ins);
        }
    }

    else if (ATmatch(fs, "Include(<str>)", &s1))
        fstateRefs2(termFromHash(parseHash(s1)), paths);

    else throw badTerm("bad fstate expression", fs);
}


void fstateRefs(FState fs, StringSet & paths)
{
    fstateRefs2(fs, paths);
}
#endif


static void parseIds(ATermList ids, FSIds & out)
{
    while (!ATisEmpty(ids)) {
        char * s;
        ATerm id = ATgetFirst(ids);
        if (!ATmatch(id, "<str>", &s))
            throw badTerm("not an id", id);
        out.push_back(parseHash(s));
        debug(s);
        ids = ATgetNext(ids);
    }
}


/* Parse a slice. */
static Slice parseSlice(FState fs)
{
    Slice slice;
    ATermList roots, elems;
    
    if (!ATmatch(fs, "Slice([<list>], [<list>])", &roots, &elems))
        throw badTerm("not a slice", fs);

    parseIds(roots, slice.roots);

    while (!ATisEmpty(elems)) {
        char * s1, * s2;
        ATermList refs;
        ATerm t = ATgetFirst(elems);
        if (!ATmatch(t, "(<str>, <str>, [<list>])", &s1, &s2, &refs))
            throw badTerm("not a slice element", t);
        SliceElem elem;
        elem.path = s1;
        elem.id = parseHash(s2);
        parseIds(refs, elem.refs);
        slice.elems.push_back(elem);
        elems = ATgetNext(elems);
    }

    return slice;
}


Slice normaliseFState(FSId id)
{
    debug(format("normalising fstate"));
    Nest nest(true);

    /* Try to substitute $id$ by any known successors in order to
       speed up the rewrite process. */
    string idSucc;
    while (queryDB(nixDB, dbSuccessors, id, idSucc)) {
        debug(format("successor %1% -> %2%") % (string) id % idSucc);
        id = parseHash(idSucc);
    }

    /* Get the fstate expression. */
    FState fs = termFromId(id);

    /* Already in normal form (i.e., a slice)? */
    if (ATgetType(fs) == AT_APPL && 
        (string) ATgetName(ATgetAFun(fs)) == "Slice")
        return parseSlice(fs);
    
    /* Then we it's a Derive node. */
    ATermList outs, ins, bnds;
    char * builder;
    char * platform;
    if (!ATmatch(fs, "Derive([<list>], [<list>], <str>, <str>, [<list>])",
            &outs, &ins, &builder, &platform, &bnds))
        throw badTerm("not a derive", fs);

    /* Right platform? */
    checkPlatform(platform);
        
    /* Realise inputs (and remember all input paths). */
    FSIds inIds;
    parseIds(ins, inIds);
    
    SliceElems inElems; /* !!! duplicates */
    StringSet inPathsSet;
    for (FSIds::iterator i = inIds.begin(); i != inIds.end(); i++) {
        Slice slice = normaliseFState(*i);
        realiseSlice(slice);

        for (SliceElems::iterator j = slice.elems.begin();
             j != slice.elems.end(); j++)
        {
            inElems.push_back(*j);
            inPathsSet.insert(j->path);
        }
    }

    Strings inPaths;
    copy(inPathsSet.begin(), inPathsSet.end(),
        inserter(inPaths, inPaths.begin()));

    /* Build the environment. */
    Environment env;
    while (!ATisEmpty(bnds)) {
        char * s1, * s2;
        ATerm bnd = ATgetFirst(bnds);
        if (!ATmatch(bnd, "(<str>, <str>)", &s1, &s2))
            throw badTerm("tuple of strings expected", bnd);
        env[s1] = s2;
        bnds = ATgetNext(bnds);
    }

    /* Check that none of the output paths exist. */
    typedef pair<string, FSId> OutPath;
    list<OutPath> outPaths;
    while (!ATisEmpty(outs)) {
        ATerm t = ATgetFirst(outs);
        char * s1, * s2;
        if (!ATmatch(t, "(<str>, <str>)", &s1, &s2))
            throw badTerm("string expected", t);
        outPaths.push_back(OutPath(s1, parseHash(s2)));
        outs = ATgetNext(outs);
    }

    for (list<OutPath>::iterator i = outPaths.begin(); 
         i != outPaths.end(); i++)
        if (pathExists(i->first))
            throw Error(format("path `%1%' exists") % i->first);

    /* Run the builder. */
    runProgram(builder, env);
        
    Slice slice;

    /* Check whether the output paths were created, and register each
       one. */
    for (list<OutPath>::iterator i = outPaths.begin(); 
         i != outPaths.end(); i++)
    {
        string path = i->first;
        if (!pathExists(path))
            throw Error(format("path `%1%' does not exist") % path);
        registerPath(path, i->second);
        slice.roots.push_back(i->second);

        Strings outPaths = filterReferences(path, inPaths);
    }

    return slice;
}


void realiseSlice(Slice slice)
{
    debug(format("realising slice"));
    Nest nest(true);

    if (slice.elems.size() == 0)
        throw Error("empty slice");

    /* Perhaps all paths already contain the right id? */

    bool missing = false;
    for (SliceElems::iterator i = slice.elems.begin();
         i != slice.elems.end(); i++)
    {
        SliceElem elem = *i;
        string id;
        if (!queryDB(nixDB, dbPath2Id, elem.path, id)) {
            if (pathExists(elem.path))
                throw Error(format("path `%1%' obstructed") % elem.path);
            missing = true;
            break;
        }
        if (parseHash(id) != elem.id)
            throw Error(format("path `%1%' obstructed") % elem.path);
    }

    if (!missing) {
        debug(format("already installed"));
        return;
    }

    /* For each element, expand its id at its path. */
    for (SliceElems::iterator i = slice.elems.begin();
         i != slice.elems.end(); i++)
    {
        SliceElem elem = *i;
        expandId(elem.id, elem.path);
    }
}
